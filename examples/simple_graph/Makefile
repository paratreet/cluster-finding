BUILD_OPTS = -I../.. -c -std=c++11 -g
CHARM_OPTS =
TEST_OPTS = 
CHARMC = ~/UIUC/charm/bin/charmc $(CHARM_OPTS)

all: graph

graph: unionFindLib.o graph.o
	$(CHARMC) -language charm++ -o $@ graph.o unionFindLib.o

graph.o : graph.C graph-io.h graph.decl.h graph.def.h unionFindLib.decl.h
	$(CHARMC) $(BUILD_OPTS) -o graph.o graph.C

graph.decl.h graph.def.h : graph.ci
	$(CHARMC) graph.ci

unionFindLib.o : unionFindLib.decl.h unionFindLib.def.h
	$(CHARMC) -o unionFindLib.o ../../unionFindLib.C

unionFindLib.decl.h unionFindLib.def.h :
	$(CHARMC) ../../unionFindLib.ci
	cp unionFindLib.de* ../../

clean:
	rm -f *.decl.h *.def.h conv-host *.o graph charmrun
	rm -f ../../*.decl.h ../../*.def.h

cleanp:
	rm -f *.sts *.gz *.projrc *.topo *.out

cleanpl:
	rm -f *.sts *.gz *.projrc *.topo *.out *.log

test-all: all
	./run_tests
