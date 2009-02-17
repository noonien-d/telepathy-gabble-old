"""
Tests requesting search channels to, and performing contact searches against,
fake servers which are broken in various ways.
"""

import dbus

from twisted.words.protocols.jabber.client import IQ

from gabbletest import exec_test
from servicetest import call_async, unwrap, make_channel_proxy, EventPattern

from pprint import pformat

import constants as cs
import ns

def call_create(q, requests, server):
    """
    Calls CreateChannel for the given contact search server, and returns the IQ
    stanza received by the server.
    """

    request = dbus.Dictionary(
        {
            cs.CHANNEL_TYPE: cs.CHANNEL_TYPE_CONTACT_SEARCH,
            cs.CONTACT_SEARCH_SERVER: server,
        }, signature='sv')
    call_async(q, requests, 'CreateChannel', request)

    iq_event = q.expect('stream-iq', to=server, query_ns=ns.SEARCH)
    return iq_event.stanza

def not_a_search_server(q, stream, requests):
    iq = call_create(q, requests, 'notajud.localhost')

    result = iq
    result['type'] = 'error'
    e = result.addElement('error')
    e['type'] = 'cancel'
    e.addElement((ns.STANZA, 'service-unavailable'))
    stream.send(result)

    event = q.expect('dbus-error', method='CreateChannel')
    assert event.error.get_dbus_name() == cs.NOT_AVAILABLE, event.error

def returns_invalid_fields(q, stream, requests):
    iq = call_create(q, requests, 'broken.localhost')

    result = IQ(stream, "result")
    result["id"] = iq["id"]
    query = result.addElement((ns.SEARCH, 'query'))
    for f in ["first", "shoe-size", "nick", "star-sign"]:
        query.addElement(f)
    stream.send(result)

    event = q.expect('dbus-error', method='CreateChannel')
    assert event.error.get_dbus_name() == cs.NOT_AVAILABLE, event.error

def returns_error_from_search(q, stream, conn, requests):
    server = 'nofunforyou.localhost'
    iq = call_create(q, requests, server)

    result = IQ(stream, "result")
    result["id"] = iq["id"]
    query = result.addElement((ns.SEARCH, 'query'))
    query.addElement("first")
    stream.send(result)

    event = q.expect('dbus-return', method='CreateChannel')
    c = make_channel_proxy(conn, event.value[0], 'Channel')
    c_search = dbus.Interface(c, cs.CHANNEL_TYPE_CONTACT_SEARCH)

    call_async(q, c_search, 'Search', {'x-n-given': 'World of Goo'})
    iq_event, _ = q.expect_many(
        EventPattern('stream-iq', to=server, query_ns=ns.SEARCH),
        EventPattern('dbus-signal', signal='SearchStateChanged'),
        )
    iq = iq_event.stanza
    iq['type'] = 'error'
    error = iq.addElement('error')
    error['type'] = 'modify'
    error.addElement((ns.STANZA, 'not-acceptable'))
    error.addElement((ns.STANZA, 'text'), content="We don't believe in games here.")
    stream.send(iq)

    ssc = q.expect('dbus-signal', signal='SearchStateChanged')
    new_state, reason, details = ssc.args

    assert new_state == cs.SEARCH_FAILED, new_state
    assert reason == cs.PERMISSION_DENIED, reason

    c.Close()

def returns_bees_from_search(q, stream, conn, requests):
    server = 'hivemind.localhost'
    iq = call_create(q, requests, server)

    result = IQ(stream, "result")
    result["id"] = iq["id"]
    query = result.addElement((ns.SEARCH, 'query'))
    query.addElement("nick")
    stream.send(result)

    event = q.expect('dbus-return', method='CreateChannel')
    c = make_channel_proxy(conn, event.value[0], 'Channel')
    c_search = dbus.Interface(c, cs.CHANNEL_TYPE_CONTACT_SEARCH)

    call_async(q, c_search, 'Search', {'nickname': 'Buzzy'})
    iq_event, _ = q.expect_many(
        EventPattern('stream-iq', to=server, query_ns=ns.SEARCH),
        EventPattern('dbus-signal', signal='SearchStateChanged'),
        )
    iq = iq_event.stanza

    result = IQ(stream, 'result')
    result['id'] = iq['id']
    result.addElement((ns.SEARCH, 'bees')).addElement('bzzzzzzz')
    stream.send(result)

    ssc = q.expect('dbus-signal', signal='SearchStateChanged')
    new_state, reason, details = ssc.args

    assert new_state == cs.SEARCH_FAILED, new_state
    assert reason == cs.NOT_AVAILABLE, reason

    c.Close()

def disconnected_before_reply(q, stream, conn, requests):
    iq = call_create(q, requests, 'slow.localhost')

    call_async(q, conn, 'Disconnect')

    event = q.expect('dbus-error', method='CreateChannel')
    assert event.error.get_dbus_name() == cs.DISCONNECTED, event.error

def test(q, bus, conn, stream):
    conn.Connect()
    q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    requests = dbus.Interface(conn, cs.CONN_IFACE_REQUESTS)

    not_a_search_server(q, stream, requests)
    returns_invalid_fields(q, stream, requests)
    returns_error_from_search(q, stream, conn, requests)
    returns_bees_from_search(q, stream, conn, requests)
    disconnected_before_reply(q, stream, conn, requests)

    q.expect('dbus-return', method='Disconnect')

if __name__ == '__main__':
    exec_test(test)
