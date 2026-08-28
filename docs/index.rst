Logos Delivery Module
=====================

The Logos Delivery Module lets your application send and receive messages over
a peer-to-peer network, without running a server of its own.

It is a Logos Core ``core`` module: it wraps
`liblogosdelivery <https://github.com/logos-messaging/logos-delivery>`_ and
exposes it to the rest of the runtime, so any other module — or a UI — can
publish to a topic, subscribe to one, or open a reliable channel by calling
methods on ``delivery_module``.

Overview
--------

A message goes through four layers on its way out:

.. code-block:: text

   your module / UI
        │  calls delivery_module methods
        ▼
   delivery_module            ← this repository
        │  C FFI
        ▼
   liblogosdelivery
        │
        ▼
   logos-delivery             ← the node implementation

In practical terms, to move a message you:

1. ``createNode`` -- build a node from a JSON configuration (once per context).
2. ``start`` -- boot it and join the network.
3. ``subscribe`` / ``send`` -- receive on a topic, publish to one.
4. ``stop`` -- shut it down.

Calls return as soon as the request is dispatched. What actually happened on
the network arrives later as an **event** — ``messageSent``,
``messagePropagated``, ``messageError``, ``messageReceived``,
``connectionStateChanged``. Subscribe to those rather than reading a return
value; see the :doc:`API Reference <api_reference>` for every method and event.

To watch this run end-to-end against a real ``logoscore`` daemon, see the
`Tutorial
<https://logos-co.github.io/logos-doctest-hub/#logos-delivery-module/ubuntu-latest/running-this-delivery-module-against-logoscore>`_.

Configuration
-------------

``createNode`` takes a JSON string, passed through verbatim to logos-delivery,
which owns the grammar. The key that shapes everything else is ``entryLayer``:
it decides how much of the stack gets mounted.

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - ``entryLayer``
     - What you get
   * - ``"kernel"``
     - Transport node only.
   * - ``"messaging"``
     - Kernel plus the messaging client.
   * - ``"channels"``
     - Kernel, messaging, and reliable channels. **The default.**

Three shapes cover almost every case.

**App developer** — the full stack, so leave ``entryLayer`` out. ``preset``
picks the network (``"logos.test"``, ``"logos.dev"``, ``"twn"``) and ``mode``
picks the protocol flags (``"Core"`` for a relay node, ``"Edge"`` for a light
one):

.. code-block:: json

   { "mode": "Core", "preset": "logos.test" }

**Node operator** — a kernel-only service node on a public network. ``mode`` is
not applied at this layer, so set the protocol flags explicitly in
``kernelConf``:

.. code-block:: json

   {
     "entryLayer": "kernel",
     "kernelConf": { "preset": "logos.test", "relay": true }
   }

**Network hoster** — a kernel-only node on a network you host yourself, where
``kernelConf`` is a raw ``WakuNodeConf`` used as-is:

.. code-block:: json

   {
     "entryLayer": "kernel",
     "kernelConf": { "clusterId": 42, "relay": true, "entryNodes": ["/dns4/…"] }
   }

On a kernel-only node, ``send``, ``subscribe`` and the ``channel*`` methods
fail with "node has no messaging client" or "no reliable channel manager".
``getNodeInfo``, ``storeQuery`` and metrics keep working.

Optional ``messagingOverrides`` and ``channelsOverrides`` objects override
per-layer defaults. Override keys accept either the config field name or its
CLI switch name — ``clusterId`` and ``cluster-id`` are the same key. The older
flat shape, with bare ``WakuNodeConf`` keys at the top level, still parses and
boots the full stack.

Content topics
--------------

A content topic names the channel a message travels on, and both publisher and
subscriber must agree on it. Use the structured format from
`LIP-23: Topics <https://lip.logos.co/messaging/informational/23/topics.html#content-topics>`_
rather than inventing one — for example ``/myapp/1/chat/proto``.

.. toctree::
   :maxdepth: 2
   :caption: Guides

   run-node
   query-node

.. toctree::
   :maxdepth: 2
   :caption: Reference

   api_reference
   versioning
