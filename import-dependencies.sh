#!/bin/bash

libzakalweurl="https://gitlab.com/hors/libzakalwe"
libzakalwehead="c665316a9dfd5a17c1b56b8979af5f5cb7b4335e"

rm -rf libzakalwe
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
