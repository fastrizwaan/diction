#!/bin/sh
IFS="
"
PATH_TO_DTD=$HOME/proj/xdxf
PATH_TO_XDXF_OUT=$HOME/proj/data/xdxf-sdict

# Loop over each XDXF file.
for xdxf in `find . -name "*.xdxf"`; do

        # Validate against DTD.
        xmlvalid --dtd=$PATH_TO_DTD/xdxf_lousy.dtd $xdxf | head -3
done