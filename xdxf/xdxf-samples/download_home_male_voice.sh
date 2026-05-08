curl -G "https://translate.google.com/translate_tts" \
    --data-urlencode "ie=UTF-8" \
    --data-urlencode "q=Home" \
    --data-urlencode "tl=en-au" \
    --data-urlencode "client=tw-ob" \
    -H "Referer: http://translate.google.com/" \
    -H "User-Agent: stagefright/1.2 (Linux;Android 5.0)" \
    -o home_male.mp3
