#!/bin/bash

export GDFONTPATH=/usr/share/fonts
export GNUPLOT_DEFAULT_GDFONT=verdana

dir=./benches
cd `dirname "$0"`
for file in `ls $dir/*.ml`; do
    ocamlopt "$file" -o "$file.opt" 2>/dev/null
    ocamlc "$file" 2>/dev/null
    ocamlclean a.out

    opt=`/usr/bin/time -f '%E' "./$file.opt" 2>&1`
    run=`/usr/bin/time -f '%E' ocamlrun a.out 2>&1`
    z3=`../bin/Z3 -o -t a.out 2>/dev/null` 


    echo "$file"
    echo -en "opt:\t"
    echo "$opt" | tail -n 1
    echo -en "run:\t"
    echo "$run" | tail -n 1
    echo -en "z3:\t"
    echo "$z3" | tail -n 1
    rm "$file.opt"
done

rm  a.out "$dir"/*.cm* "$dir"/*.o
