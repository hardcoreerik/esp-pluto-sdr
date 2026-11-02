"""Exercise the actual TCP adapter against a fragmented loopback IIOD peer."""
import socket, subprocess, threading, time
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0)); listener.listen()
    errors = []
    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(2)
                command = b''
                while not command.endswith(b'\n'): command += conn.recv(1)
                assert command == b'PRINT\r\n', command
                for byte in b'4\n<x/>\n':
                    conn.sendall(bytes([byte])); time.sleep(.001)
        except Exception as e: errors.append(e)
    worker = threading.Thread(target=serve, daemon=True); worker.start()
    subprocess.run(['/tmp/pluto-tcp-test', str(listener.getsockname()[1])], check=True, timeout=3)
    worker.join(3)
    assert not worker.is_alive() and not errors, errors
print('TCP loopback check passed')
