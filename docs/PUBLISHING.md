# Publishing Guide

This guide helps you publish safely without leaking WiFi credentials or API keys.

## 1) Keep secrets local
- Use `include/secrets.example.h` for placeholders only.
- Keep real credentials only in `include/secrets.h`.
- `include/secrets.h` is ignored by git in `.gitignore`.

## 2) Run a quick secret scan
From project root:

```powershell
Get-ChildItem -Recurse -File |
  Where-Object { $_.FullName -notmatch '\\.pio\\' } |
  Select-String -Pattern 'WIFI_SSID|WIFI_PASSWORD|OWM_API_KEY|apikey|api_key|password|ssid'
```

Expected result before publish:
- No real credentials found.
- Placeholder strings are acceptable in `secrets.example.h`.

## 3) Publish
```powershell
git add .
git commit -m "Initial public release"
git push -u origin main
```

## 4) After publish
- Regenerate your API key immediately if a secret was ever committed by mistake.
- If a secret reached remote history, rotate credentials and rewrite history before continuing.
