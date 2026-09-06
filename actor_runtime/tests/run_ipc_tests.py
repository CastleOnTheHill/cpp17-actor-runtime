"""Spawn real separate C processes; inject malformed Unix datagrams before valid traffic."""
import os
import selectors
import socket
import subprocess
import sys

binary = sys.argv[1]
names = [f'ipc_test_{os.getpid()}_a', f'ipc_test_{os.getpid()}_b']
servers = []
try:
    for index, name in enumerate(names):
        server = subprocess.Popen([binary, 'server', name, str(index + 1)], stdout=subprocess.PIPE, text=True)
        servers.append(server)
        selector = selectors.DefaultSelector()
        selector.register(server.stdout, selectors.EVENT_READ)
        if not selector.select(10):
            raise RuntimeError('server failed to announce readiness')
        line = server.stdout.readline().strip()
        selector.close()
        if line != 'READY':
            raise RuntimeError(f'server startup failed: {line}')
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as attacker:
        attacker.bind(f'\0test_malformed_{os.getpid()}')
        address = f'\0actor-runtime.{os.getuid()}.{names[0]}'
        for packet in [b'', b'x', bytes(248), bytes(65535)]:
            attacker.sendto(packet, address)
    subprocess.run([binary, 'client', *names], check=True, timeout=20)
    for server in servers:
        output, _ = server.communicate(timeout=5)
        print(output, end='')
        if server.returncode:
            raise RuntimeError(f'server exit {server.returncode}')
finally:
    for server in servers:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
