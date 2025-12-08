import xmlrpc.client
import os
import sys

def send_file(server_ip, server_port, filepath):
    if not os.path.isfile(filepath):
        print(f"Error: File '{filepath}' not found.")
        return

    filename = os.path.basename(filepath)
    
    server_url = f"http://{server_ip}:{server_port}"
    print(f"Connecting to RPC Server at {server_url}")
    
    try:
        proxy = xmlrpc.client.ServerProxy(server_url)

        with open(filepath, "rb") as f:
            file_content = f.read()

        print(f"Sending file: {filename} ({len(file_content)} bytes)...")        
        response = proxy.save_file(filename, xmlrpc.client.Binary(file_content))
        print(f"Server replied: {response}")

    except ConnectionRefusedError:
        print("Error: Could not connect to the server. Is it running?")
    except Exception as e:
        print(f"RPC Error: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python client.py <server_ip> <server_port> <file_path>")
        print("Example: python client.py 127.0.0.1 5000 test.txt")
        sys.exit(1)
    
    server_ip = sys.argv[1]
    server_port = int(sys.argv[2])
    filepath = sys.argv[3]
        
    send_file(server_ip, server_port, filepath)