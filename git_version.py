import subprocess

Import("env")
TEMPLATE = '''#define GIT_VERSION "NOGIT"'''
GIT_CMD = ["/usr/bin/git", "describe", "--long", "--dirty"]

git_proc = subprocess.run(GIT_CMD, stdout=subprocess.PIPE)
version = git_proc.stdout.decode().rstrip()
with open('include/version.h', 'w') as header_file:
    header_file.write(TEMPLATE.replace('NOGIT', version))
