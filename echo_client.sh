nodemon -w out/echo_client -x sh -- -c 'while :; do timeout 10 ./out/echo_client tcp||true; sleep 1; done'
