/*
 * Server main function
 *
 * Copyright (C) 1998 Alexandre Julliard
 */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "server.h"
#include "server/object.h"

int main( int argc, char *argv[] )
{
    int fd;
    extern void server_main_loop( int fd );

    if (argc != 2) goto error;
    if (!isdigit( *argv[1] )) goto error;
    fd = atoi( argv[1] );
    /* make sure the fd is valid */
    if (fcntl( fd, F_GETFL, 0 ) == -1) goto error;

    debug_level = 1;

    if (debug_level) printf( "Server: starting (pid=%d)\n", getpid() );
    server_main_loop( fd );
    if (debug_level) printf( "Server: exiting (pid=%d)\n", getpid() );
    exit(0);

 error:    
    fprintf( stderr, "%s: must be run from Wine.\n", argv[0] );
    exit(1);
}
