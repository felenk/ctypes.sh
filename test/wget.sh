#!/bin/bash

source ../ctypes.sh

declare -r AF_UNSPEC=int:0
declare -r SOCK_STREAM=int:1

struct addrinfo hints;
struct addrinfo result;

# This is used to dereference a pointer
declare -a nativeptr=(pointer)

# Request to send
declare hostname="www.google.com"
declare port="80"
declare request=$'GET / HTTP/1.1\r\nHost: www.google.com\r\n\r\n'

hints[ai_family]=$AF_UNSPEC
hints[ai_socktype]=$SOCK_STREAM
hints[ai_flags]=int:0
hints[ai_protocol]=int:0

# Allocate space for a native structure.
dlcall -r pointer -n hintsptr malloc $(sizeof addrinfo)

# Allocate space for a pointer
dlcall -r pointer -n resultptr malloc $(sizeof long)

# Translate hints to native structure.
pack $hintsptr hints

# Call getaddrinfo()
dlcall -r int -n s getaddrinfo string:$hostname string:$port $hintsptr $resultptr

# Check result, and print error if necessary. Note that if you want c-style
# escapes in bash, you need to use $'xxx', not "xxx".
if [[ $s != int:0 ]]; then
    dlcall -r pointer -n gaierror gai_strerror $s
    dlcall printf $'getaddrinfo returned an error, %s\n' $gaierror
    dlcall free $hinstrptr
    dlcall free $resultptr
    exit 1
fi

# Translate that result into bash structure.
unpack $resultptr nativeptr
unpack $nativeptr result

# getaddrinfo returns a linked list, try each one until one works.
while true; do
    # Attempt to connect to this address
    dlcall -r int -n sfd socket ${result[ai_family]} ${result[ai_socktype]} ${result[ai_protocol]}
    dlcall -r int -n ret connect $sfd ${result[ai_addr]} ${result[ai_addrlen]}

    if [[ $ret == int:0 ]]; then
        echo successfully connected to ${result[ai_addr]}...
        break
    fi

    # close unused socket
    dlcall close $sfd

    echo connect to ${result[ai_addr]} failed, trying ${result[ai_next]}...

    # Check if this is the last result
    if [[ ${result[ai_next]} == $NULL ]]; then
        break
    fi

    # Move to the next element of list
    unpack ${result[ai_next]} result
    set | grep ^result=
done

dlcall freeaddrinfo $nativeptr
dlcall free $hinstrptr
dlcall free $resultptr

if [[ $ret != int:0 ]]; then
    echo "unable to connect to any address, giving up..."
    exit 1
fi

# Send a GET request and read 1024 bytes of response.
dlcall -r pointer -n buf calloc 1024 1
dlcall -r int -n ret write $sfd string:"${request}" ${#request}
dlcall -r int -n ret read $sfd $buf 1023

dlcall close $sfd

# Check if that worked
if [[ $ret != int:-1 ]]; then
    dlcall puts $buf
    printf "\n<result truncated>\n"
    dlcall free $buf
    exit 0
fi

dlcall free $buf
exit 1