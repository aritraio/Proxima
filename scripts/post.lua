-- scripts/post.lua -- wrk upload-heavy script (DESIGN 11.3).
-- Generates a fixed 64 KiB body with Content-Length on each request.
-- Usage: wrk -t4 -c200 -d30s -s scripts/post.lua http://127.0.0.1:8080/post
wrk.method = "POST"
wrk.path = "/post"
wrk.headers["Content-Type"] = "application/octet-stream"
-- 64 KiB of 'x' (wrk keeps conns alive; body reused per request)
wrk.body = string.rep("x", 65536)
