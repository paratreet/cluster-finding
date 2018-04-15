## Parallel Cluster Finding Library

This library provides the functionality of performing cluster-finding
on graphs in a distributed asynchronous fashion. The implementation is in
Charm++ and can be used in any generic Charm++-based graph applications.

### Example

An example application (a simple graph program) is included in the `examples`
directory. A more detailed documentation of the library usage and functionality
will be added soon.

### Currently implemented features

* Fully distributed asynchronous union-find algorithm
* Local and global path compression optimizations
* Connected components identification & labelling
* Threshold-based component pruning
* Optimizations for tree path lengths and message aggregation

