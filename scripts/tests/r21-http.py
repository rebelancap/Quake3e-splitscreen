# usage: python r21-http.py <port> <directory> [bytes per second]
# R21: a static HTTP server for the map-download test (r21-dl.sh), optionally throttled so
# the download screen can be captured.  Logs every request (path, status, bytes sent).
import http.server, os, sys, time

PORT = int(sys.argv[1]); ROOT = sys.argv[2]; RATE = int(sys.argv[3]) if len(sys.argv) > 3 else 0

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def copyfile(self, source, outputfile):
        sent = 0
        while True:
            buf = source.read(65536)
            if not buf:
                break
            outputfile.write(buf)
            sent += len(buf)
            if RATE:
                time.sleep(len(buf) / RATE)
        sys.stderr.write("sent %d bytes for %s (User-Agent: %s)\n" % (sent, self.path, self.headers.get("User-Agent")))

print("r21-http: serving %s on 127.0.0.1:%d, rate %s" % (ROOT, PORT, RATE or "unlimited"), flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
