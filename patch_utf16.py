import re

files_to_patch = ['src/dict-loader.c', 'src/dict-dsl-scanner.c']

for filepath in files_to_patch:
    with open(filepath, 'r') as f:
        content = f.read()

    # The bug is that unmatched surrogates are encoded directly.
    # We just need to insert a check:
    # if (wc >= 0xD800 && wc <= 0xDFFF) wc = 0xFFFD;
    # right before `if (wc < 0x80)`

    new_content = content.replace(
        "if (wc < 0x80) { out_buf[out++] = wc; }",
        "if (wc >= 0xD800 && wc <= 0xDFFF) { wc = 0xFFFD; }\n        if (wc < 0x80) { out_buf[out++] = wc; }"
    )

    with open(filepath, 'w') as f:
        f.write(new_content)

print("Patched utf16 converters.")
