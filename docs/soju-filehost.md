# Image uploads to your own soju (FILEHOST)

soju 0.8 and newer can store files that clients upload (`soju.im/FILEHOST`). VitaIRC 1.1 uses it
when **Settings → Image service** is **soju FILEHOST**: the image goes to the bouncer of the window
you are in, authenticated with the same account and password you use for SASL.

## soju configuration

```text
file-upload fs /var/lib/soju/uploads
http-ingress https://bnc.example.com        # FILEHOST = <http-ingress>/uploads
listen http://localhost:8080                 # behind a reverse proxy on the same machine
accept-proxy-ip localhost
message-store db                             # also needed for chathistory
```

Create the upload directory, owned by the user soju runs as, and restart soju.

Reverse proxy (nginx) inside the HTTPS `server` block of the domain:

```nginx
location /uploads {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto https;
    client_max_body_size 50m;
}
```

Without a proxy, soju can serve HTTPS itself: `listen https://:8443` (with the `tls` directive) and
`http-ingress https://bnc.example.com:8443`.

## Test it

```sh
curl -i -u 'user:password' -H 'Content-Type: image/png' \
  -H 'Content-Disposition: attachment; filename="t.png"' \
  --data-binary @t.png https://bnc.example.com/uploads
```

Expect `201 Created` and a `Location` header. When VitaIRC connects, the server window shows
*"This server accepts image uploads (soju FILEHOST)"*.

## Notes

- If the IRC connection uses TLS, VitaIRC refuses a plain `http://` FILEHOST.
- A self-signed certificate is accepted only when the FILEHOST is on the same host as the IRC server.
- VitaIRC sends the username without the `/network@client` suffix.
- soju does not delete uploads; a daily `find /var/lib/soju/uploads -type f -mtime +30 -delete` keeps the disk tidy.
