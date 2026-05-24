import re

with open("/var/home/rizvan/diction/src/dict-xdxf.c", "r") as f:
    orig = f.read()

# Extract from #include to xdxf_flush_pending_space
idx = orig.find("static void process_xml_xdxf")
header = orig[:idx]

# Extract the inner loop from process_xml_xdxf
loop_start = orig.find("while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > ar_depth)")
loop_end = orig.find("g_string_append(def_str, \"</div>\");", loop_start)

loop_body = orig[loop_start:loop_end]
# Remove the initial while condition and brace
loop_body = re.sub(r'while\s*\(xmlTextReaderRead\(reader\) == 1 && xmlTextReaderDepth\(reader\) > ar_depth\)\s*\{', '', loop_body, 1)
# Remove the last closing brace
loop_body = loop_body.rstrip()
if loop_body.endswith('}'):
    loop_body = loop_body[:-1]

# In loop_body, replace 'state->xdxf_standard' with 'dict->xdxf_standard'
loop_body = loop_body.replace("state->xdxf_standard", "dict->xdxf_standard")
# replace ar_lousy_format with dict->xdxf_lousy_format
loop_body = loop_body.replace("ar_lousy_format", "dict->xdxf_lousy_format")

# We don't need 'state->default_lousy_format' logic since the index will store it. Wait, the old code checked '<ar>' 'f' attribute.
# In the raw chunk, the root element IS the '<ar>' element! We can read it.
# Actually, the loop we extracted doesn't have the '<ar>' attributes!
# So we must parse the '<ar>' attributes first.

