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

# URLs for Bryce and Alba (Fixed Raw Links)
BRYCE_ONNX="https://sfo3.digitaloceanspaces.com/bkmdls/bryce.onnx"
BRYCE_JSON="https://sfo3.digitaloceanspaces.com/bkmdls/bryce.onnx.json"

# Note: Using "resolve" instead of "blob" to get the actual file data
ALBA_ONNX="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/alba/medium/en_GB-alba-medium.onnx"
ALBA_JSON="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/alba/medium/en_GB-alba-medium.onnx.json"

# 4. Preparation
mkdir -p "$PIPER_DIR"
mkdir -p "$OUTPUT_DIR"
cd "$PIPER_DIR" || exit

# 5. Download Models
echo "--- Syncing Models ---"
wget -q -N "$BRYCE_ONNX" "$BRYCE_JSON"
wget -q -N "$ALBA_ONNX" "$ALBA_JSON"

# 6. Processing Function
generate_opus() {
    local MODEL=$1
    local REGION=$2
    local FINAL_NAME="${REGION}-${WORD}.opus"
    
    # Check if model exists to prevent the JSON crash
    if [ ! -f "$MODEL" ]; then
        echo "Error: Model file $MODEL not found!"
        return
    fi
    
    echo "Generating [$WORD] for $REGION..."
    
    # Generate WAV via Piper
    echo "$WORD" | ./piper/piper --model "$MODEL" --output_file temp.wav
    
    # Convert using mpv's ffmpeg wrapper
    flatpak run --filesystem=host --command=ffmpeg io.mpv.Mpv -y -i temp.wav \
        -c:a libopus -b:a 32k "$FINAL_NAME"

    # Move to samples folder
    if [ -f "$FINAL_NAME" ]; then
        mv "$FINAL_NAME" "$OUTPUT_DIR/"
    fi
    rm -f temp.wav
}

# Ensure piper is executable
chmod +x "./piper/piper" 2>/dev/null

# 7. Execution check
if [ -x "./piper/piper" ]; then
    generate_opus "bryce.onnx" "en_US"
    # Ensure this matches the filename downloaded by wget
    generate_opus "en_GB-alba-medium.onnx" "en_UK"
    echo "---"
    echo "Success: Files for '$WORD' moved to $OUTPUT_DIR"
else
    echo "Error: Piper binary not found in $PIPER_DIR/piper"
    exit 1
fi
