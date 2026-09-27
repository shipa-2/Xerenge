#!/usr/bin/env python3
import os
import shutil
import socket
import struct
import subprocess
import time
import sys

def encode_message(cmd: str, fields_dict: dict, code: str = "\0\0\0\0") -> bytes:
    body = ""
    for k, v in fields_dict.items():
        body += f"{k}={v}\n"
    body += "\0"
    b_body = body.encode("utf-8")
    b_cmd = cmd.encode("ascii")[:4].ljust(4, b"\0")
    b_code = code.encode("ascii")[:4].ljust(4, b"\0")
    total_len = 12 + len(b_body)
    header = b_cmd + b_code + struct.pack(">I", total_len)
    return header + b_body

def decode_message(sock: socket.socket) -> tuple[str, str, dict]:
    header = sock.recv(12)
    if len(header) < 12:
        raise ValueError(f"Incomplete header: {header}")
    cmd = header[:4].decode("ascii", errors="replace")
    code = header[4:8].decode("ascii", errors="replace")
    total_len = struct.unpack(">I", header[8:12])[0]
    body_len = total_len - 12
    body = b""
    while len(body) < body_len:
        chunk = sock.recv(body_len - len(body))
        if not chunk:
            break
        body += chunk
    body_str = body.decode("utf-8", errors="replace")
    fields = {}
    for line in body_str.split("\n"):
        line = line.strip("\0\r")
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k] = v
    return cmd, code, fields

def find_ealobby_bin():
    if "EALOBBY_BIN" in os.environ and os.path.isfile(os.environ["EALOBBY_BIN"]):
        return os.environ["EALOBBY_BIN"]
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(script_dir, "../build/ealobby"),
        os.path.join(script_dir, "../../bin/ealobby"),
        shutil.which("ealobby"),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return "ealobby"

def run_tests():
    dir_port = 31862
    lobby_port = 31863
    proc = subprocess.Popen([
        find_ealobby_bin(),
        "--directory-port", str(dir_port),
        "--lobby-port", str(lobby_port)
    ])
    time.sleep(0.5)

    try:
        print("[TEST] 1. Connect to Directory Server...")
        s_dir = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_dir.connect(("127.0.0.1", dir_port))
        s_dir.sendall(encode_message("@dir", {}))
        cmd, code, fields = decode_message(s_dir)
        assert cmd == "@dir", f"Expected @dir, got {cmd}"
        assert "PORT" in fields and fields["PORT"] == str(lobby_port), f"Bad fields: {fields}"
        print(f"       Directory replied with ADDR={fields['ADDR']}, PORT={fields['PORT']}")
        s_dir.close()

        print("[TEST] 2. Connect Client 1 to Lobby Server...")
        c1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c1.connect(("127.0.0.1", lobby_port))

        c1.sendall(encode_message("addr", {}))
        cmd, _, _ = decode_message(c1)
        assert cmd == "addr"

        c1.sendall(encode_message("skey", {}))
        cmd, _, fields = decode_message(c1)
        assert cmd == "skey" and "SKEY" in fields
        print(f"       SKEY exchange OK: {fields['SKEY']}")

        c1.sendall(encode_message("news", {}))
        cmd, _, fields = decode_message(c1)
        assert cmd == "news"

        print("[TEST] 3. Client 1 Auth & Persona...")
        c1.sendall(encode_message("auth", {"NAME": "PlayerOne"}))
        cmd, _, fields = decode_message(c1)
        assert cmd == "auth" and fields["NAME"] == "PlayerOne"

        c1.sendall(encode_message("pers", {"PERS": "PlayerOne"}))
        cmd, _, fields = decode_message(c1)
        assert cmd == "pers" and fields["PERS"] == "PlayerOne"

        # Check async +who
        cmd, _, fields = decode_message(c1)
        assert cmd == "+who" and fields["N"] == "PlayerOne"
        print(f"       +who received: Persona={fields['N']}, ID={fields['I']}")

        c1.sendall(encode_message("sele", {"ROOMS": "1", "GAMES": "1"}))
        cmd, _, _ = decode_message(c1)
        assert cmd == "sele"

        # sele triggers +who if ROOMS/STATS
        cmd, _, _ = decode_message(c1)
        assert cmd == "+who"

        print("[TEST] 4. Client 1 Create Game...")
        c1.sendall(encode_message("gcre", {"NAME": "Revenge Grand Prix", "PARAMS": "track=1", "USERPARAMS": "car=revenge_special"}))
        cmd, _, fields = decode_message(c1)
        assert cmd == "gcre" and fields["NAME"] == "Revenge Grand Prix"
        game_id = fields["IDENT"]
        print(f"       Game created: IDENT={game_id}, HOST={fields['HOST']}")

        print("[TEST] 5. Connect Client 2...")
        c2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c2.connect(("127.0.0.1", lobby_port))
        c2.sendall(encode_message("skey", {}))
        decode_message(c2)
        c2.sendall(encode_message("auth", {"NAME": "PlayerTwo"}))
        decode_message(c2)
        c2.sendall(encode_message("pers", {"PERS": "PlayerTwo"}))
        decode_message(c2)
        decode_message(c2) # +who

        print("[TEST] 6. Client 2 Search Games & Join...")
        c2.sendall(encode_message("gsea", {}))
        cmd, _, fields = decode_message(c2)
        assert cmd == "gsea" and int(fields["COUNT"]) >= 1
        cmd, _, fields = decode_message(c2)
        assert cmd == "+gam" and fields["IDENT"] == game_id
        print(f"       Found game: {fields['NAME']}")

        c2.sendall(encode_message("gjoi", {"IDENT": game_id, "USERPARAMS": "car=muscle_type_1"}))
        cmd, _, fields = decode_message(c2)
        assert cmd == "gjoi" and fields["COUNT"] == "2"
        print(f"       Client 2 joined game! Player count: {fields['COUNT']}")

        # Client 1 should receive +gam notification of player joining
        cmd, _, fields = decode_message(c1)
        assert cmd == "+gam" and fields["COUNT"] == "2"
        print("       Client 1 received +gam notification of Client 2 joining!")

        print("[TEST] 7. Client 2 Ready (gset)...")
        c2.sendall(encode_message("gset", {"USERFLAGS": "1"}))
        cmd, _, fields = decode_message(c2)
        assert cmd == "gset"

        # Client 1 receives update
        cmd, _, fields = decode_message(c1)
        assert cmd == "+gam"

        print("[TEST] 8. Client 1 Starts Race (gsta)...")
        c1.sendall(encode_message("gsta", {}))
        cmd, _, _ = decode_message(c1)
        assert cmd == "gsta"

        # Both clients should receive +ses!
        cmd1, _, fields1 = decode_message(c1)
        assert cmd1 == "+ses" and fields1["IDENT"] == game_id
        print("       Client 1 received +ses (Session Started)!")

        cmd2, _, fields2 = decode_message(c2)
        assert cmd2 == "+ses" and fields2["IDENT"] == game_id
        print("       Client 2 received +ses (Session Started)!")

        print("\nALL MULTIPLAYER LOBBY PROTOCOL TESTS PASSED! 🎉")

        c1.close()
        c2.close()

    finally:
        proc.terminate()
        proc.wait()

if __name__ == "__main__":
    run_tests()
