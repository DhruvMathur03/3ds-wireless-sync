import socket, pathlib, shlex, sys

HOST = "192.168.0.139"     # 3DS IP
PORT = 8080
DL_DIR = pathlib.Path.home() / "Downloads"

with socket.create_connection((HOST, PORT)) as s:
    hdr = b''
    while not hdr.endswith(b'\n'):
        hdr += s.recv(1)

    parts = shlex.split(hdr.decode().strip())
    if len(parts) != 3 or parts[0] != 'FILE':
        print("Bad header:", hdr.decode().strip())
        sys.exit()

    name, size = parts[1], int(parts[2])
    print(f"Receiving {name} ({size} bytes)…")

    data = bytearray()
    while len(data) < size:
        chunk = s.recv(4096)
        if not chunk: break
        data.extend(chunk)
DL_DIR.mkdir(parents=True, exist_ok=True)
out_path = DL_DIR / name
out_path.write_bytes(data)
print("Saved to : ", out_path)
