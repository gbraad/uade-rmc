#!/bin/bash

libzakalweurl="https://gitlab.com/hors/libzakalwe"
libzakalwehead="ac84680f86aea88c9219d6be7482916e83fff0c7"

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
