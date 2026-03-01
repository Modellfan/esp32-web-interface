#!/usr/bin/env python3
import argparse
import functools
import http.server
import pathlib
import socketserver
import webbrowser


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Serve local web UI files for the interactive docs demo."
    )
    parser.add_argument("--port", type=int, default=8080, help="Port to bind (default: 8080)")
    parser.add_argument(
        "--directory",
        default="data",
        help="Directory to serve (default: data)",
    )
    parser.add_argument(
        "--open-browser",
        action="store_true",
        help="Open a browser automatically (default: disabled)",
    )
    args = parser.parse_args()

    root_dir = pathlib.Path(args.directory).resolve()
    if not root_dir.exists() or not root_dir.is_dir():
        raise SystemExit(f"Directory does not exist: {root_dir}")

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(root_dir))

    class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    server = ThreadingServer(("127.0.0.1", args.port), handler)
    base_url = f"http://127.0.0.1:{args.port}"
    demo_url = f"{base_url}/interactive-docs.html"

    print(f"Serving: {root_dir}")
    print(f"Base URL: {base_url}")
    print(f"Demo URL: {demo_url}")
    print("Press Ctrl+C to stop.")

    if args.open_browser:
        webbrowser.open(demo_url, new=2)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
