Internal docs
=============

How the module is put together, and how to operate a node built from it.

- :doc:`Architecture <architecture>` -- where the module sits in the stack and
  how much of it a given node mounts.
- :doc:`Running a node <run-node>` -- Docker, prebuilt binaries or Nix.
- :doc:`Querying a node <query-node>` -- reading identity, version and metrics
  off a running node.
- :doc:`Versioning <versioning>` -- what a version bump means and how a
  release is cut.

.. toctree::
   :hidden:
   :maxdepth: 1

   architecture
   run-node
   query-node
   versioning
