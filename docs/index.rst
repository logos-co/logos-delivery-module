Logos Delivery Module
=====================

.. note::

   | The main Logos Messaging documentation is at
     `docs.logos.co/messaging <https://docs.logos.co/messaging>`_.
   | Start there for the concepts and the wider stack.

   **This site is the API reference** and the internal docs.

   To watch this run end-to-end against a real ``logoscore`` daemon, see the
   `Tutorial
   <https://logos-co.github.io/logos-doctest-hub/#logos-delivery-module/ubuntu-latest/running-this-delivery-module-against-logoscore>`_.


The Logos Delivery Module lets your application send and receive messages over
a peer-to-peer network, without running a server of its own. It is a Logos Core
``core`` module: it wraps
`liblogosdelivery <https://github.com/logos-messaging/logos-delivery>`_ and
exposes it to the rest of the runtime, so any other module — or a UI — can
publish to a topic, subscribe to one, or open a reliable channel by calling
methods on ``delivery_module``.

Using the API
-------------

1. ``createNode`` -- build a node from a JSON configuration (once per context).
2. ``start`` -- boot it and join the network.
3. ``subscribe`` / ``send`` -- receive on a topic, publish to one.
4. ``stop`` -- shut it down.

Calls return as soon as the request is dispatched. What actually happened on
the network arrives later as an **event** — subscribe to those rather than
reading a return value.

API Reference
-------------

- :doc:`Methods <pages/api_reference>`
- :doc:`Events <pages/events>`

Internal docs
-------------

- :doc:`Architecture <pages/architecture>`
- :doc:`Running a node <pages/run-node>`
- :doc:`Querying a node <pages/query-node>`
- :doc:`Versioning <pages/versioning>`

.. Hidden: the lists above are the visible index; these only build the sidebar
   nav and the page hierarchy.

.. toctree::
   :hidden:
   :maxdepth: 1
   :caption: API reference

   pages/api_reference
   pages/events

.. toctree::
   :hidden:
   :maxdepth: 1
   :caption: Internal docs

   pages/architecture
   pages/run-node
   pages/query-node
   pages/versioning
