import socket
import struct
import os
import sys

BUFFER_SIZE = 4096

def send_file(server_ip, server_port, filename):
    if not os.path.isfile(filename):
        print(f"File '{filename}' does not exist.")
        return

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        print(f"Connecting to {server_ip}:{server_port}...")
        sock.connect((server_ip, server_port))
        print("Connected.")

        file_basename = os.path.basename(filename)
        filename_bytes = file_basename.encode('utf-8')
        filesize = os.path.getsize(filename)

        sock.sendall(struct.pack('!I', len(filename_bytes)))
        sock.sendall(filename_bytes)
        sock.sendall(struct.pack('!Q', filesize))

        print(f"Sending file: {file_basename} ({filesize} bytes)")

        with open(filename, 'rb') as f:
            while True:
                chunk = f.read(BUFFER_SIZE)
                if not chunk:
                    break
                sock.sendall(chunk)

        print("File transmission complete.")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python client.py <server_ip> <server_port> <filename>")
        sys.exit(1)

    server_ip = sys.argv[1]
    server_port = int(sys.argv[2])
    filename = sys.argv[3]

    send_file(server_ip, server_port, filename)