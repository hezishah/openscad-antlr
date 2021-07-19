rm -R scad
antlr scad.g4 -o scad
pushd $(pwd)
cd scad
javac -classpath $(which antlr| echo $(dirname $(which antlr))/$(dirname $(xargs readlink))/../antlr-4.9.2-complete.jar) scad*.java
#echo "module hezi () \n { \n }" | grun scad prog -gui
grun scad prog -gui < ../DoorStop.scad
popd