Logos Delivery Module
=====================

.. note::

   The main Logos Messaging documentation is at
   `docs.logos.co/messaging <https://docs.logos.co/messaging>`_. Start there
   for the concepts, the wider stack, and how Delivery and Chat fit together.

   **This site is the API reference** for the ``delivery_module`` Logos Core
   module, together with the internal docs for building, running and querying
   a node.

The Logos Delivery Module lets your application send and receive messages over
a peer-to-peer network, without running a server of its own. It is a Logos Core
``core`` module: it wraps
`liblogosdelivery <https://github.com/logos-messaging/logos-delivery>`_ and
exposes it to the rest of the runtime, so any other module — or a UI — can
publish to a topic, subscribe to one, or open a reliable channel by calling
methods on ``delivery_module``.

Using the API
-------------

To move a message you:

1. ``createNode`` -- build a node from a JSON configuration (once per context).
2. ``start`` -- boot it and join the network.
3. ``subscribe`` / ``send`` -- receive on a topic, publish to one.
4. ``stop`` -- shut it down.

Calls return as soon as the request is dispatched. What actually happened on
the network arrives later as an **event** — subscribe to those rather than
reading a return value.

:doc:`API Reference <api_reference>` has every method, including the full
``createNode`` configuration grammar. :doc:`Events <events>` has the full set
of events and their payloads.

To watch this run end-to-end against a real ``logoscore`` daemon, see the
`Tutorial
<https://logos-co.github.io/logos-doctest-hub/#logos-delivery-module/ubuntu-latest/running-this-delivery-module-against-logoscore>`_.

.. toctree::
   :maxdepth: 2
   :caption: API reference

   api_reference
   events

.. toctree::
   :maxdepth: 2
   :caption: Internal docs

   architecture
   run-node
   query-node
   versioning
