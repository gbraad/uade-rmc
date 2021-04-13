#!/bin/bash

libzakalweurl="https://gitlab.com/hors/libzakalwe"
libzakalwehead="790abe7124e134b82aadbf7a3fac4b15d8c079a6"

if [[ ! -e libzakalwe ]] ; then
    while true ; do
	echo -n "Do you trust to import ${libzakalweurl} ${libzakalwehead}? (y/n) "
	read answer
	if [[ ${answer} = "y" ]] ; then
	    break;
	elif [[ ${answer} = "n" ]] ; then
	    echo "Abort"
	    exit 1
	fi
    done
    git clone -n ${libzakalweurl} && cd libzakalwe && git reset --hard ${libzakalwehead}
    if [[ $? != "0" ]] ; then
	echo "Clone failed"
	cd ..
	rm -rf libzakalwe
    fi
fi