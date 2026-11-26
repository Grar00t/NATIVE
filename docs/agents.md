# Run agents while files are being written

From the checkout, build with `make build/think` on Linux or `tools\build-think.cmd` in an x64 Visual Studio developer prompt. The agent runner links the C11 task loop and C++17 filesystem code. It uses the standard libraries and operating-system APIs.

## Monitor a build

Run this command from the repository root. The input paths are examples. The directory containing the files must exist.

```sh
build/think run --root agent --trigger publish --task build-17 \
  --bind artifact=size:output.bin --bind writer=file:writer.status \
  --timeout-ms 30000 --poll-ms 50 --progress stdout
```

In a second terminal, write some data, then publish the writer's status.

```sh
printf 'payload' > output.bin
printf 'ready\n' > writer.next
mv writer.next writer.status
```

The bundled `build-started` agent observes the first byte. The dependent `artifact-ready` agent waits for the writer's `ready` record. The file size alone says nothing about whether a producer has finished. Use a fresh pair of input paths per task when old outputs can exist. The task ID labels events. It does not select or reset input files.

## Drop in another agent

Create `agent/footer-seen/agent.ini` with these contents. An agent's predicate lives in its manifest. This example waits for the producer to publish `sealed`. To install it while a run is active, write `agent.ini.tmp` in the new folder and rename it to `agent.ini`, or rename an entire prepared directory into the root. Use the same atomic replacement procedure for edits.

```ini
version = 1
name = footer-seen
trigger = release
requires = artifact-ready
input = footer
op = equals
value = sealed
fail = invalid
```

Select it with `--agent footer-seen` or `--trigger release`. Keep the two bindings used by its prerequisites and add `--bind footer=file:footer.status`. For an API client or queue consumer that writes newline-terminated status values, pipe its stdout to the runner and use `--bind footer=stdin`. The producing process owns the connection and its credentials.

| Field | Meaning |
|---|---|
| `version` | The supported grammar version is `1`. |
| `name` | Must match the enclosing directory. Names, triggers and binding identifiers start with an ASCII letter or digit and can also contain hyphens and underscores. |
| `trigger` | An event name selected by the caller. |
| `requires` | Optional whitespace-separated agent names. The runner evaluates their predicates first, even if they declare other triggers. |
| `input` | Logical capability bound by the operator through `--bind NAME=SOURCE`. |
| `op`, `value` | `equals` matches literal status text. `at-least` compares unsigned 64-bit decimal integers, including zero. |
| `fail` | Optional literal value that fails the task before the success predicate is applied. |

Each nonblank, noncomment line sets one field using the first equals sign. Whitespace around keys and values is ignored. A line starting with `#` is a comment. Duplicate fields, unsupported versions and unknown predicates fail with a diagnostic. The manifest contains data and has no command or expression interpolation.

For a read-only GitHub example, give an agent the input `policy`, predicate `equals`, value `false` and failure value `true`. Bind `policy=stdin` and pipe `gh api "repos/$repo/actions/permissions" --jq '.enabled'` into the runner. Set `repo` to the owner/repository being checked. The same agent can read a saved response with `--bind policy=file:policy.txt`. One request publishes one observation. A producer can send subsequent observations on the same pipe.

## Connectors and task lifetime

| Binding | Wire format or observation |
|---|---|
| `file:PATH` | One status value committed with LF or CRLF. A missing file or unfinished line stays pending. Extra records are an error. |
| `size:PATH` | Current byte length of a regular file, including zero. The runner reads metadata while the producer writes. |
| `stdin` | Newline-terminated snapshots from a pipe or redirected file. Each poll retains the last complete record it consumed. A silent pipe remains pending. EOF retains the last complete record. |
| `--progress stdout`, `stderr`, `file:PATH` | One flushed UTF-8 JSON object per state or value change, plus task start and terminal events. File output appends. Standard output is the default. |

Invoke `run` once per task. A named run watches its agent and transitive dependencies. A trigger run discovers all matching manifests each poll and finishes when at least one matches and every match and its prerequisites are ready in that pass. A missing agent or prerequisite can arrive during the run. A cycle or missing connector binding is an error. A published manifest replacement takes effect on the next poll. Readiness is re-evaluated each pass, so input changes can revoke it. The caller can start the runner before any agent is installed.

The root and all file addresses come from the invocation. Relative paths resolve from the caller's working directory. Tasks require positive poll and timeout intervals. The C loop uses a monotonic clock and caps its sleep at the remaining deadline. Polling samples file state and can miss a change that reverts between polls. There is no transaction across input files. A stalled filesystem operation or blocked progress sink can delay a return from the operating system. Use regular files on a responsive filesystem and drain any output pipe.

| Exit | Meaning |
|---|---|
| 0 | The selected agent or trigger completed. |
| 1 | An input, manifest, dependency, explicit failure record or output operation failed. |
| 2 | Invalid invocation. |
| 124 | The task deadline expired. |

`agent.ini`, version 1, transport names and exit codes are protocol constants. The reader caps a manifest at 16 KiB, a status record at 4096 bytes before LF and a dependency path at 128 agents to bound memory and call-stack use. Durations are 1 through 4,294,967,294 ms. Windows reserves the next timer value for an infinite wait. The 400 ms refresh belongs to the original interactive stream meter. Its 32 KiB copy buffer bounds stream memory and has no effect on task polling.

`tools/think.h` defines the C connector callbacks for embedding the same bounded loop in another host. The C++ host binds source addresses to readers in `open_input` and routes progress through `Progress`. A new native transport extends that host. A new predicate using the documented operators requires only a manifest. Run the drop-in, reload, timeout, failure and connector tests with `make check-agents`.
