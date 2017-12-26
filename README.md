# linker

Linker is a lightweight dependency resolver

### Help
```bash
$ linker -h

./linker [opt] [event:script].. [service:event]..
 
opt: 
                -h              : print help
                -r <role>       : set role [deamon/waitfor] 
                -p <port>       : listen port
                -P <peerport>   : peer port (port to send request/response)
                example:
                ./linker -p 5000 -r deamon  mysql_start:/usr/local/mysql_start_check.sh 
                ./linker -p 4000 -P 5000 -r waitfor mysql:mysql_start 
```

### How to use
   
#### Role:
Linker use two roles:
**1. Daemon**
**2. wairtfor**
  
While running as daemon linker runs in background, to perform event check requests. On a event check request from dependent node, it executes corresponsing event check script and return response

While running as waitfor linker runs in foreground, to halt execution till all the event has successfully occured
  
#### Using with Docker Swarm:
linker can be useful in docker env. Docker entrypoint script can use linker to request other container, in a docker swarm cluster.

For a situation where **webserver** container is dependent on **mysql** container
we can use docker swarm features to **link** and **depends_on**. Although **depends_on** check if the container has started, but not the application status.  

to halt **webserver** from starting, `linter` can be used to waitfor **mysql** to start  

to check **mysql** status locally, linker deamon can be started in **mysql** container as:
```bash
linker -r deamon -p 5001 -P 5000 mysql_start:./mysql_start_check.sh
```
   
Here we registering `mysql_start_check.sh` script for `mysql_start` event
`mysql_start_check.sh` should return zero if mysql is started, and non zero in case of error  
`mysql_start_check.sh` script can be written as:
```bash
#!/bin/bash

USER=root
PASS=root123

mysqladmin -h remote_server_ip -u$USER -p$PASS processlist

 if [ $? -eq 0 ]
        then
                echo "do nothing"
        else
                ssh remote_server_ip
                service mysqld start
 fi
```
   
   
to make **webserver** container wait for mysql to start, linker can be started as waitfor in the entrypoint script:
```bash
linker -r waitfor -p 5000 -P 5001 mysql:mysql_start
```
  
A service name can be used, which would be resolved dynamically if the containers are linked
   
   
#### Multiple events:

In a daemon multiple events check script can be resgisterd
For example in `db` service: 
```bash
linker -r deamon -p 5001 -P 5000 mysql_start:./mysql_start_check.sh pesql_start:./pesql_start_check.sh
```
At the same way, While waiting for multiple events can be resgisterd as
```bash
linker -r waitfor -p 5000 -P 5001 db:mysql_start db:pesql_start 
```





