"""Exercise the packaged Windows MCP server through its standard streams."""

from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import json
import subprocess
import sys
import tempfile


def request(request_id, method, **params):
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


def tool(request_id, tool_name, **arguments):
    return request(request_id, "tools/call", name=tool_name, arguments=arguments)


def write(path, text):
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def exchange(executable, vault, messages):
    completed = subprocess.run(
        [str(executable), "--mcp", "--vault", str(vault)],
        input="".join(json.dumps(m, ensure_ascii=False) + "\n" for m in messages),
        encoding="utf-8", capture_output=True, timeout=20, check=True,
    )
    return [json.loads(line) for line in completed.stdout.splitlines()]


def check(executable, vault):
    for area in (".keeper", "Projects", "Backups", "Trash"):
        (vault / area).mkdir()
    for name, fields in {
        "Alpha": "description=Ordinary project",
        "Zéta": "description=Pinned fixture\npinned=1\nrepo=example/project",
        "Hidden": "hidden=1\ndescription=Hidden fixture",
        "Locked": "locked=1\ndescription=Locked fixture",
    }.items():
        folder = vault / "Projects" / name
        folder.mkdir()
        write(folder / ".projectkeeper", fields + "\n")
    project = vault / "Projects" / "Zéta"
    write(project / "PROJECT_NOTES.md", 'A🧰B\nLine with "quotes".\n')
    write(project / ".projectkeeper_issues", "repo=example/project\nissue.12=1Cached title\n")

    # Simulate the native app's exclusive write lock for the entire MCP session.
    kernel = ctypes.WinDLL("kernel32.dll", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    create.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    lock = create(str(vault / ".keeper" / "write.lock"), 0xC0000000, 0, None, 4, 128, None)
    if lock == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    messages = [
        request(1, "initialize", protocolVersion="2025-11-25",
                capabilities={}, clientInfo={"name": "smoke", "version": "1"}),
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        request(2, "tools/list"),
        tool(3, "list_projects"),
        tool(4, "list_projects", query="ZÉTA", pinned_only=True),
        tool(5, "get_project", project="Zéta", include_stats=True),
        tool(6, "read_project_document", project="Zéta", document="notes", limit=2),
        tool(7, "read_project_document", project="Zéta", document="notes", offset=1),
        tool(8, "list_project_issues", project="Zéta"),
        tool(9, "get_project", project="Hidden"),
        tool(10, "get_project", project="Locked"),
        tool(11, "get_project", project="..\\.keeper"),
        tool(12, "create_project", name="Blocked by writer"),
    ]
    try:
        replies = exchange(executable, vault, messages)
    finally:
        kernel.CloseHandle(lock)
    assert [r["id"] for r in replies] == list(range(1, 13)), replies
    results = {r["id"]: r["result"] for r in replies}
    assert results[1]["protocolVersion"] == "2025-11-25"
    assert len(results[2]["tools"]) == 7
    data = {i: results[i]["structuredContent"] for i in range(3, 9)}
    assert [p["name"] for p in data[3]["projects"]] == ["Zéta", "Alpha"]
    assert [p["name"] for p in data[4]["projects"]] == ["Zéta"]
    assert data[5]["pinned"] and data[5]["source"]["files"] >= 1
    assert data[6]["text"] == "A" and data[6]["next_offset"] == 1
    assert data[7]["text"] == '🧰B\nLine with "quotes".\n'
    assert data[8]["issues"] == [{"number": 12, "title": "Cached title", "open": True}]
    assert all(results[i]["isError"] for i in (9, 10, 11, 12))
    assert "busy" in results[12]["content"][0]["text"]

    with tempfile.TemporaryDirectory(prefix="project-keeper-source-") as folder:
        source = Path(folder)
        write(source / "README.md", "Original source.\n")
        written = exchange(executable, vault, [
            tool(20, "create_project", name="Imported", source_folder=str(source), description="Imported via MCP"),
            tool(21, "create_project", name="New project"),
            tool(22, "create_project", name="Imported"),
            tool(23, "write_project_notes", project="Imported", text="Release notes"),
            tool(24, "update_project", project="Imported", description="New description", pinned=True),
            tool(25, "write_project_notes", project="Locked", text="Disallowed"),
            tool(26, "update_project", project="Hidden", pinned=True),
            tool(27, "read_project_document", project="Imported", document="notes"),
            tool(28, "write_project_notes", project="Imported", text="Stale", mode="replace", expected_revision=data[6]["revision"]),
        ])
        result = {r["id"]: r["result"] for r in written}
        assert not any(result[i].get("isError") for i in (20, 21, 23, 24, 27))
        assert result[20]["structuredContent"]["imported"]
        assert all(result[i]["isError"] for i in (22, 25, 26, 28))
        assert result[24]["structuredContent"]["pinned"] and result[24]["structuredContent"]["description"] == "New description"
        imported = vault / "Projects" / "Imported"
        assert (imported / "README.md").read_bytes() == (source / "README.md").read_bytes()
        assert "New description" in (imported / "PROJECT_CONTEXT.md").read_text(encoding="utf-8")
        assert "Release notes" in result[27]["structuredContent"]["text"]
        revision = result[27]["structuredContent"]["revision"]
        update = exchange(executable, vault, [tool(
            29, "write_project_notes", project="Imported", mode="replace", text="Updated notes.",
            expected_revision=revision
        )])[0]["result"]
        assert update["structuredContent"]["changed"]
        assert (imported / "PROJECT_NOTES.md").read_text(encoding="utf-8") == "Updated notes."
        assert (vault / ".keeper" / "mcp-change").is_file()

        def append(line):
            return exchange(executable, vault, [tool(30, "write_project_notes", project="Imported", text=line)])[0]["result"]

        with ThreadPoolExecutor(max_workers=2) as clients:
            assert all(result["structuredContent"]["changed"] for result in clients.map(append, ["First writer", "Second writer"]))
        assert set((imported / "PROJECT_NOTES.md").read_text(encoding="utf-8").splitlines()) == {
            "Updated notes.", "First writer", "Second writer"
        }


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="project-keeper-mcp-") as folder:
        check(Path(sys.argv[1]).resolve(), Path(folder))
    print("PASS: MCP stdio, seven tools, concurrent writes, revision checks, and protected-project filtering")
