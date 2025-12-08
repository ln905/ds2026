import os
import sys
from xmlrpc.server import SimpleXMLRPCServer
import xmlrpc.client

UPLOAD_DIR = "received_files"

def save_file(filename, binary_data):
    if not isinstance(binary_data, xmlrpc.client.Binary):
        return "ERROR: Data must be xmlrpc.client.Binary"

    if not os.path.exists(UPLOAD_DIR):
        os.makedirs(UPLOAD_DIR)

    safe_filename = os.path.basename(filename)
    dest_path = os.path.join(UPLOAD_DIR, safe_filename)

    with open(dest_path, "wb") as f:
        f.write(binary_data.data)

    print(f"Saved: {safe_filename} ({len(binary_data.data)} bytes)")
    return f"OK: Saved {safe_filename}"

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python server.py <port>")
        sys.exit(1)

    port = int(sys.argv[1])
    

    print(f"Server listening on 0.0.0.0:{port}")
    server = SimpleXMLRPCServer(('0.0.0.0', port), allow_none=True)
    server.register_function(save_file, 'save_file')
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")