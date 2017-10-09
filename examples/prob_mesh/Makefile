BUILD_OPTS = -I../.. -c -std=c++11 -g
CHARM_OPTS = -tracemode projections
TEST_OPTS = ++local
CHARMC = charmc $(CHARM_OPTS)

all: mesh

mesh: unionFindLib.o mesh.o
	$(CHARMC) -language charm++ -o $@ mesh.o unionFindLib.o

mesh.o : mesh.C mesh.decl.h mesh.def.h unionFindLib.decl.h
	$(CHARMC) $(BUILD_OPTS) -o mesh.o mesh.C

mesh.decl.h mesh.def.h : mesh.ci
	$(CHARMC) mesh.ci

unionFindLib.o : unionFindLib.decl.h unionFindLib.def.h
	$(CHARMC) -o unionFindLib.o ../../unionFindLib.C

unionFindLib.decl.h unionFindLib.def.h :
	$(CHARMC) ../../unionFindLib.ci
	cp unionFindLib.de* ../../

clean:
	rm -f *.decl.h *.def.h conv-host *.o mesh charmrun
	rm -f ../../*.decl.h ../../*.def.h

cleanp:
	rm -f *.sts *.gz *.projrc *.topo *.out

cleanpl:
	rm -f *.sts *.gz *.projrc *.topo *.out *.log

test: all
	./charmrun +p4 ./mesh 26 2 0.4
