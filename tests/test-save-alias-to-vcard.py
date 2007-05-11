
"""
Regression test.

 - the 'alias' connection parameter is set
 - our vCard doesn't have a NICKNAME field
 - we crash when trying to save a vcard with NICKNAME set to the alias
   parameter
"""

import dbus

from twisted.internet import glib2reactor
glib2reactor.install()

from twisted.words.xish import domish
from twisted.internet import reactor

from gabbletest import EventTest, conn_iface, gabble_test_setup, run

def expect_connected(event, data):
    if event[0] != 'dbus-signal':
        return False

    if event[2] != 'StatusChanged':
        return False

    if event[3] != [0, 1]:
        return False

    return True

def expect_get_vcard(event, data):
    if event[0] != 'stream-iq':
        return False

    # Looking for something like this:
    #   <iq xmlns='jabber:client' type='get' id='262286393608'>
    #      <vCard xmlns='vcard-temp'/>

    iq = event[1]

    if iq['type'] != 'get':
        return False

    if iq.uri != 'jabber:client':
        return False

    vcard = list(iq.elements())[0]

    if vcard.name != 'vCard':
        return False

    # Send empty vCard back.
    iq['type'] = 'result'
    data['stream'].send(iq)
    return True

def expect_set_vcard(event, data):
    if event[0] != 'stream-iq':
        return False

    iq = event[1]

    if iq['type'] != 'set':
        return False

    if iq.uri != 'jabber:client':
        return False

    vcard = list(iq.elements())[0]

    if vcard.name != 'vCard':
        return False

    nickname = vcard.firstChildElement()
    assert nickname.name == 'NICKNAME'
    assert str(nickname) == 'Some Guy'
    conn_iface(data['conn']).Disconnect()
    return True

def expect_disconnected(event, data):
    if event[0] != 'dbus-signal':
        return False

    if event[2] != 'StatusChanged':
        return False

    if event[3] != [2, 1]:
        return False

    return True

if __name__ == '__main__':
    test = EventTest()
    map(test.expect, [
        expect_connected,
        expect_get_vcard,
        expect_set_vcard,
        expect_disconnected,
        ])
    run(test, {'alias': 'Some Guy'})

