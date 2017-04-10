#!/bin/bash

http_load_dir=http_load-09Mar2016
package=$http_load_dir.tar.gz

_install()
{
    wget http://www.acme.com/software/http_load/$package
    tar -xvf $package
    cd $http_load_dir
    make
    cd ..
}

_test()
{
    $http_load_dir/http_load -proxy 127.0.0.1:3128 -rate 5 -seconds 10 -parallel 5 ./urls 
}

_main()
{
    if [ "$1" = "install" ]; then
        _install
    elif [ "$1" = "test" ]; then
        _test
    else
        _help $0
        exit 1
    fi
    exit 0
}

_main $1
