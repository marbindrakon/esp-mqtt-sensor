
#!/usr/bin/python3

# Modified from Mozilla certstore script by Earle F. Philhower, III.  Released to the public domain.

from __future__ import print_function
import csv
import os
import sys
from subprocess import Popen, PIPE, call
try:
    from urllib.request import urlopen
except:
    from urllib2 import urlopen
try:
    from StringIO import StringIO
except:
    from io import StringIO

pem_files = os.listdir("tls_trust_anchors")
# Try and make ./data, skip if present
try:
    os.mkdir("data")
except:
    pass

derFiles = []

# Process the text PEM using openssl into DER files
for pemFile in pem_files:
    certName = "data/{0}".format(pemFile).replace('pem', 'der')
    print(pemFile + " -> " + certName)
    ssl = Popen(['openssl','x509','-inform','PEM','-in', os.path.join('tls_trust_anchors', pemFile), '-outform','DER','-out', certName])
    ssl.wait()
    if os.path.exists(certName):
        derFiles.append(certName)

if os.path.exists("data/certs.ar"):
    os.unlink("data/certs.ar");

arCmd = ['ar', 'q', 'data/certs.ar'] + derFiles;
call( arCmd )

for der in derFiles:
    os.unlink(der)
