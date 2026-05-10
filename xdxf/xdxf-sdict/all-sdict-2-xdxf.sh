#!/bin/sh
IFS="
"
PATH_TO_DTD=$HOME/proj/xdxf
PATH_TO_XDXF_OUT=$HOME/proj/data/xdxf-sdict

# Loop over each SDICT file.
for dct in `find . -name "*.dct"`; do
        #echo $dct

        # Convert SDICT to XDXF.
        makedict -o xdxf --work-dir $PATH_TO_XDXF_OUT $dct

        # Strip path.
        name=`echo $dct | sed s/.dct//g | sed 's/\/.*\///g' | sed 's/^\.//g' | sed 's/^\///g'`
        #echo -- $name --

        # Validate against DTD.
        xmlvalid --dtd=$PATH_TO_DTD/xdxf_lousy.dtd $PATH_TO_XDXF_OUT/$name/dict.xdxf | head

        # Strip spaces from dir name.
        name_ns=`echo $name | sed 's/ /_/g'`
        if [ $name != $name_ns ]; then
          mv $PATH_TO_XDXF_OUT/$name $PATH_TO_XDXF_OUT/$name_ns
        fi
done