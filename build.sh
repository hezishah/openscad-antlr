rm -R scad
antlr scad.g4 -o scad
pushd $(pwd)
cd scad
javac -classpath /opt/brew//Cellar/antlr/4.9.1/antlr-4.9.1-complete.jar scad*.java
#echo "module hezi () \n { \n }" | grun scad prog -gui
grun scad prog -gui < ../DoorStop.scad
popd