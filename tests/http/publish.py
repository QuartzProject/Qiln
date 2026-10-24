"""Publishes the bundle plugin to a Maven repository served over HTTP on 127.0.0.1, then resolves it from there."""

import base64
import functools
import hashlib
import http.server
import os
import pathlib
import shutil
import subprocess
import sys
import threading

AUTH = "Basic " + base64.b64encode(b'ada:pa"ss\\word').decode()


class Repository(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_PUT(self):
        if self.headers.get("Authorization") != AUTH:
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="test"')
            self.end_headers()
            return
        target = pathlib.Path(self.translate_path(self.path))
        if self.headers.get("If-None-Match") == "*" and target.exists():
            self.send_response(412)
            self.end_headers()
            return
        body = self.rfile.read(int(self.headers["Content-Length"]))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(body)
        self.send_response(201)
        self.end_headers()


def qiln(binary, cwd, *args, ok=True):
    run = subprocess.run([binary, *args], cwd=cwd, capture_output=True, text=True)
    if (run.returncode == 0) != ok:
        sys.exit(f"qiln {' '.join(args)} in {cwd} exited with {run.returncode}\n{run.stdout}\n{run.stderr}")
    return run.stdout + run.stderr


def expect(text, output):
    if text not in output:
        sys.exit(f"expected {text!r} in:\n{output}")


def main():
    binary, examples, work = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(examples, work, ignore=shutil.ignore_patterns("build", ".qiln", "qiln.lock"))
    served = work / "served"
    served.mkdir()
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(Repository, directory=str(served)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_address[1]}/releases"

    home = work / "home"
    home.mkdir()
    os.environ["QILN_HOME"] = str(home)
    (home / "signing.key").write_text("07" * 32 + "\n")
    (home / "credentials.toml").write_text(f"[test]\nurl = \"{url}\"\nusername = \"ada\"\npassword = 'pa\"ss\\word'\n")

    expect("plain http is refused", qiln(binary, work / "bundle", "publish", "--to", url, ok=False))
    out = qiln(binary, work / "bundle", "publish", "--to", url, "--allow-insecure-localhost")
    expect(f"published org.example.bundle 1.0.0 (build-plugin", out)
    out = qiln(binary, work / "bundle", "publish", "--to", url, "--allow-insecure-localhost", ok=False)
    expect("already published", out)

    version = served / "releases/org/example/bundle/1.0.0"
    archive = (version / "bundle-1.0.0.qzs").read_bytes()
    if (version / "bundle-1.0.0.qzs.sha256").read_text() != hashlib.sha256(archive).hexdigest():
        sys.exit("the .sha256 beside the archive does not match it")
    expect("<version>1.0.0</version>", (served / "releases/org/example/bundle/maven-metadata.xml").read_text())

    settings = work / "shop/settings.qz"
    text = settings.read_text().replace(
        's.includePlugin("../bundle")',
        f's.pluginRepository(Repository.maven("{url}"))\n    s.plugin("org.example.bundle:^1.0")')
    settings.write_text(text)
    out = qiln(binary, work / "shop", "bundle", "--allow-insecure-localhost")
    expect("locked org.example.bundle 1.0.0 (build-plugin)", out)
    expect("wrote build/distributions/greeter-1.0.0.bundle", out)
    server.shutdown()


if __name__ == "__main__":
    main()
