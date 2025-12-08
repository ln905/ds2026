from mpi4py import MPI
import os
import sys

UPLOAD_DIR = "received_files"
CHUNK_SIZE = 4096
TAG_METADATA = 1
TAG_DATA = 2

def run_sender(filename, dest_rank, comm):
    if not os.path.isfile(filename):
        print(f"[Sender] Error: File '{filename}' not found.")
        comm.send(None, dest=dest_rank, tag=TAG_METADATA)
        return

    file_basename = os.path.basename(filename)
    filesize = os.path.getsize(filename)
    metadata = {'filename': file_basename, 'filesize': filesize}

    print(f"[Sender] Sending metadata: {metadata} to Rank {dest_rank}")
    comm.send(metadata, dest=dest_rank, tag=TAG_METADATA)
    print(f"[Sender] Transmission started...")
    sent_bytes = 0
    with open(filename, 'rb') as f:
        while True:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break 

            comm.send(chunk, dest=dest_rank, tag=TAG_DATA)
            sent_bytes += len(chunk)
    
    comm.send(None, dest=dest_rank, tag=TAG_DATA)
    print(f"[Sender] Transfer complete. Total sent: {sent_bytes} bytes.")


def run_receiver(source_rank, comm):
    print(f"[Receiver] Waiting for connection from Rank {source_rank}...")
    metadata = comm.recv(source=source_rank, tag=TAG_METADATA)
    
    if metadata is None:
        print("[Receiver] Error: Sender aborted the operation.")
        return

    filename = metadata['filename']
    filesize = metadata['filesize']

    if not os.path.exists(UPLOAD_DIR):
        os.makedirs(UPLOAD_DIR)
    
    dest_path = os.path.join(UPLOAD_DIR, filename)
    print(f"[Receiver] Incoming file: {filename} ({filesize} bytes)")

    received_bytes = 0
    with open(dest_path, 'wb') as f:
        while True:
            chunk = comm.recv(source=source_rank, tag=TAG_DATA)
            if chunk is None:
                break
            
            f.write(chunk)
            received_bytes += len(chunk)
            
    print(f"[Receiver] File saved to: {dest_path}")
    print(f"[Receiver] Verification: {received_bytes}/{filesize} bytes received.")

if __name__ == "__main__":
    comm = MPI.COMM_WORLD
    rank = comm.Get_rank() 
    size = comm.Get_size() 

    if size < 2:
        if rank == 0:
            print("Error: This program requires at least 2 MPI processes.")
            print("Usage: mpiexec -n 2 python mpi_transfer.py <filename>")
        sys.exit(1)

    if rank == 0:
        if len(sys.argv) < 2:
            print("Usage: mpiexec -n 2 python mpi_transfer.py <filename>")
            comm.send(None, dest=1, tag=TAG_METADATA) 
        else:
            filename = sys.argv[1]
            run_sender(filename, dest_rank=1, comm=comm)
            
    elif rank == 1:
        run_receiver(source_rank=0, comm=comm)