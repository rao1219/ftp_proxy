How to run our proxy program:

1. Modify the "FTP_ADDR" and "PROXYIP" in source code header.
   FTP_ADDR is your FTP server and client ip address,PROXYIP is your proxy program's location ip address(here we use linux virtual machine).

2. Compile:
   gcc proxy.c -o proxy

3. Run:
   sudo ./proxy
   Since it bind 21 port as command socket, root authentication is required.