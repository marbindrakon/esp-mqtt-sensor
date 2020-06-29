import subprocess

Import("env")
TEMPLATE = '''#define GIT_VERSION "NOGIT"'''
GIT_CMD = ["/usr/bin/git", "describe", "--long", "--dirty"]

git_proc = subprocess.run(GIT_CMD, capture_output=True)
version = git_proc.stdout.decode().rstrip()
with open('include/version.h', 'w') as header_file:
    header_file.write(TEMPLATE.replace('NOGIT', version))
