import pathlib

source = pathlib.Path(__file__).with_name("d2_bridge.c").read_text()
stack = []
line = 1
i = 0
state = "code"
while i < len(source):
    c = source[i]
    n = source[i + 1] if i + 1 < len(source) else ""
    if c == "\n":
        line += 1
    if state == "code":
        if c == "/" and n == "*":
            state = "block"
            i += 1
        elif c == "/" and n == "/":
            state = "line"
            i += 1
        elif c == '"':
            state = "string"
        elif c == "'":
            state = "char"
        elif c == "{":
            stack.append(line)
        elif c == "}":
            if not stack:
                raise SystemExit("extra closing brace at line %d" % line)
            stack.pop()
    elif state == "block" and c == "*" and n == "/":
        state = "code"
        i += 1
    elif state == "line" and c == "\n":
        state = "code"
    elif state in ("string", "char"):
        if c == "\\":
            i += 1
        elif (state == "string" and c == '"') or (state == "char" and c == "'"):
            state = "code"
    i += 1

if stack:
    raise SystemExit("unclosed braces at lines: %s" % stack)
print("C_BRACE_TEST_OK")
