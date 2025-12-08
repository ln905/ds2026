import socket
import struct
import os
import sys

OUTPUT_DIR = "received_files" 
BUFFER_SIZE = 4096

def start_server(port):
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    server_sock.bind(('0.0.0.0', port))
    server_sock.listen(1)
    print(f"Server listening on port {port}")

    try:
        while True:
            print("Waiting for a connection...")
            conn, addr = server_sock.accept()
            print(f"Connected from {addr}")

            try:
                raw_len = conn.recv(4)
                if not raw_len: break
                name_len = struct.unpack('!I', raw_len)[0]
                filename_bytes = conn.recv(name_len)
                filename = filename_bytes.decode('utf-8')
                filename = os.path.basename(filename) 
                raw_size = conn.recv(8)
                filesize = struct.unpack('!Q', raw_size)[0]

                print(f"Receiving file: {filename} ({filesize} bytes)")
                output_path = os.path.join(OUTPUT_DIR, filename)
                received_bytes = 0

                with open(output_path, 'wb') as f:
                    while received_bytes < filesize:
                        chunk_size = min(BUFFER_SIZE, filesize - received_bytes)
                        data = conn.recv(chunk_size)
                        if not data: break
                        f.write(data)
                        received_bytes += len(data)
                print(f"File saved to {output_path}")

            except Exception as e:
                print(f"Error: {e}")
            finally:
                conn.close()
                print("Connection closed.")

    except KeyboardInterrupt:
        print("\nServer stopping...")
    finally:
        server_sock.close()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python server.py <port>")
        sys.exit(1)
    
    port = int(sys.argv[1])
    start_server(port)