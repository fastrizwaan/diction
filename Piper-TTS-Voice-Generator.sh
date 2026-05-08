#!/bin/bash

# 1. Help Function
show_help() {
    echo "Piper TTS Generator for diction-c"
    echo ""
    echo "Usage: $0 [word] or [option]"
    echo ""
    echo "Options:"
    echo "  -h, --help    Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 apple      Generates en_US-apple.opus and en_UK-apple.opus"
}

# 2. Argument Handling
case "$1" in
    -h|--help|"")
        show_help
        exit 0
        ;;
    *)
        WORD="$1"
        ;;
esac

# 3. Configuration
PIPER_DIR="$HOME/tts"
OUTPUT_DIR="$HOME/tts/output"

# URLs for Bryce and Cori
BRYCE_ONNX="https://sfo3.digitaloceanspaces.com/bkmdls/bryce.onnx"
BRYCE_JSON="https://sfo3.digitaloceanspaces.com/bkmdls/bryce.onnx.json"
CORI_ONNX="https://sfo3.digitaloceanspaces.com/bkmdls/cori-high.onnx"
CORI_JSON="https://sfo3.digitaloceanspaces.com/bkmdls/cori-high.onnx.json"

# 4. Preparation
mkdir -p "$PIPER_DIR"
mkdir -p "$OUTPUT_DIR"
cd "$PIPER_DIR" || exit

# 5. Download Models (Silent but efficient)
echo "--- Syncing Models ---"
wget -q -N "$BRYCE_ONNX" "$BRYCE_JSON"
wget -q -N "$CORI_ONNX" "$CORI_JSON"

# 6. Processing Function
generate_opus() {
    local MODEL=$1
    local REGION=$2
    local FINAL_NAME="${REGION}-${WORD}.opus"
    
    echo "Generating [$WORD] for $REGION..."
    
    # Generate WAV via Piper
    echo "$WORD" | ./piper/piper --model "$MODEL" --output_file temp.wav
    
    # Convert using mpv's ffmpeg wrapper
    # Note: Using 'ffmpeg' command via io.mpv.Mpv flatpak
    flatpak run --filesystem=host --command=ffmpeg io.mpv.Mpv -y -i temp.wav \
        -c:a libopus -b:a 32k "$FINAL_NAME"

    # Move to samples folder
    mv "$FINAL_NAME" "$OUTPUT_DIR/"
    rm temp.wav
}

chmod +rx "./piper/piper" 
# 7. Execution check
if [ -x "./piper/piper" ]; then
    generate_opus "bryce.onnx" "en_US"
    generate_opus "cori-high.onnx" "en_UK"
    echo "Success: Files moved to $OUTPUT_DIR"
else
    echo "Error: Piper binary not found or not executable in $PIPER_DIR/piper"
    exit 1
fi
