# Style Guide

## Error handling

All error handling goes through `throw.go` — a thin panic/recover wrapper that turns Go's two-value error returns into exception-style flow.

### Primitives

- `Throw(err error)` — if `err != nil`, panic with an `*Exception`.
- `Throw2[T](val T, err error) T` — unwraps `(val, err)` returns; re-throws on error, otherwise returns `val`.
- `Throw3[T1,T2](v1 T1, v2 T2, err error) (T1, T2)` — three-value version.
- `ThrowFmt(format, args...)` — like `Throw(fmt.Errorf(...))` but unconditional; for raising our own errors.
- `Fmt(format, args...) *Exception` — construct an exception without throwing.
- `Try(cb func()) *Exception` — catch-all. Runs `cb`, converts `*Exception` panics into returned values, lets any other panic propagate.
- `(*Exception).Catch(cb)` — if non-nil, call `cb` with it. Fluent error handler at the boundary.
- `(*Exception).AsError() error` — interoperate with stdlib/3rd-party APIs that want an `error`.

### Rule

**No `if err != nil { return err }` in application code.** Wrap calls in `Throw2`/`Throw` instead:

```go
// BAD
f, err := os.Open(path)
if err != nil {
    return err
}

// GOOD
f := Throw2(os.Open(path))
```

Catches belong at boundaries:

- `main.go`: the top-level `Try(func(){...}).Catch(...)` prints the error and `os.Exit(1)`.
- Each goroutine's entry function: wrap the body in `Try(...)` and report via `Catch` — otherwise a panic escapes the goroutine and kills the process.
- Any filter-style loop where a per-iteration failure should be skipped, not propagated: wrap the iteration body in `Try` and ignore.

### When `if err != nil` is allowed

Local, non-propagating uses are fine:

- **Filter**: `if err != nil { continue }` to skip a bad item in a loop (e.g. a per-packet write that may transiently fail).
- **Discriminate**: checking `errors.Is`/`errors.As` to recognise an expected case (e.g. `net.ErrClosed` to decide whether to log-and-continue or escalate).

The forbidden shape is a pure **pass-through** — `if err != nil { return err }` that does nothing except re-type the bubble. Use `Throw` for that.

### When a function should return `error`

Returning an `error` is fine — even encouraged — when the error is **part of the function's contract** and the caller is expected to branch on it, not just propagate it further. Typical cases:

- **Interface obligation**: e.g. `io.Reader.Read`, `io.Writer.Write`. The stdlib contract requires a returned `error`; don't wrap it in `Throw` to spite the signature.
- **Domain signal that drives a branch**: the error is an outcome the caller discriminates on.
- **Top-level lifecycle return**: `func (g *Gofra) Run() error` exposes the first fatal goroutine error to `main`, which decides between log+exit and silent shutdown.

The distinction is: does the caller do something *specific* with this error, or does it just `return err`? If the latter — you're passing through, use `Throw`. If the former — return the error; the value carries meaning.

## Formatting

### Blank lines around control blocks

Before and after `if`, `for`, `switch`, `select`, `go func`, `defer func` — add a blank line.

Exception: no blank line if the block is the first or last statement inside `{}`.

```go
func foo() {
    if cond {              // first stmt, no blank before
        return
    }
                           // blank after
    doThing()
                           // blank before for
    for _, x := range xs {
        use(x)
    }
}                          // for was last stmt, no blank after
```

### Blank lines before `return`

Always add a blank line before `return`.

Exception: no blank line if `return` is the first statement after `{`.

```go
func empty() int {
    return 0               // first stmt, no blank
}

func nonEmpty() int {
    x := compute()
                           // blank before return
    return x
}
```

### Logical grouping

Consecutive one-liners (`Throw*`, `defer`, `:=`, `=`) that form a single logical operation stay together without blank lines. Between separate logical operations — add a blank line.

Example — opening a TUN and bringing the link up is one operation:

```go
f := Throw2(openTUNFD(dev, true))
defer f.Close()

link := Throw2(netlink.LinkByName(dev))
Throw(netlink.LinkSetUp(link))
```

## Project layout

All `.go` files live in the repo root. No `internal/`, no `cmd/`, no `pkg/`. The project is small; directory hierarchy would be overhead, not structure.

## Config

JSON only. No YAML, ever.

## Dependencies

- `github.com/vishvananda/netlink` — TUN device IP/MTU/up.
- `golang.org/x/sys/unix` — raw syscalls (`recvmmsg`, `setsockopt`, `ioctl`).
- standard library otherwise.

The shape is: native Go bindings for kernel surfaces, no shelling out.
