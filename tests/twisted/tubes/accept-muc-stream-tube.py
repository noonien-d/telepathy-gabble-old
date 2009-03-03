"""Test IBB stream tube support in the context of a MUC."""

import dbus

from servicetest import call_async, EventPattern, EventProtocolClientFactory, unwrap
from gabbletest import make_result_iq, acknowledge_iq
import constants as cs
import ns
import tubetestutil as t
from bytestream import create_from_si_offer

from twisted.words.xish import domish, xpath
from twisted.internet import reactor

sample_parameters = dbus.Dictionary({
    's': 'hello',
    'ay': dbus.ByteArray('hello'),
    'u': dbus.UInt32(123),
    'i': dbus.Int32(-123),
    }, signature='sv')

def test(q, bus, conn, stream, bytestream_cls):
    conn.Connect()

    _, iq_event = q.expect_many(
        EventPattern('dbus-signal', signal='StatusChanged', args=[0, 1]),
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'))

    acknowledge_iq(stream, iq_event.stanza)

    call_async(q, conn, 'RequestHandles', 2,
        ['chat@conf.localhost'])

    event = q.expect('stream-iq', to='conf.localhost',
            query_ns='http://jabber.org/protocol/disco#info')
    result = make_result_iq(stream, event.stanza)
    feature = result.firstChildElement().addElement('feature')
    feature['var'] = 'http://jabber.org/protocol/muc'
    stream.send(result)

    event = q.expect('dbus-return', method='RequestHandles')
    handles = event.value[0]
    room_handle = handles[0]

    # join the muc
    call_async(q, conn, 'RequestChannel', cs.CHANNEL_TYPE_TEXT, cs.HT_ROOM,
        room_handle, True)

    _, stream_event = q.expect_many(
        EventPattern('dbus-signal', signal='MembersChanged',
            args=[u'', [], [], [], [2], 0, 0]),
        EventPattern('stream-presence', to='chat@conf.localhost/test'))

    # Send presence for other member of room.
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/bob'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'owner'
    item['role'] = 'moderator'
    stream.send(presence)

    # Send presence for own membership of room.
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/test'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'none'
    item['role'] = 'participant'
    stream.send(presence)

    q.expect('dbus-signal', signal='MembersChanged',
            args=[u'', [2, 3], [], [], [], 0, 0])

    assert conn.InspectHandles(1, [2]) == ['chat@conf.localhost/test']
    assert conn.InspectHandles(1, [3]) == ['chat@conf.localhost/bob']
    bob_handle = 3

    event = q.expect('dbus-return', method='RequestChannel')

    # Bob offers a stream tube
    stream_tube_id = 666
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/bob'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'owner'
    item['role'] = 'moderator'
    tubes = presence.addElement((ns.TUBES, 'tubes'))
    tube = tubes.addElement((None, 'tube'))
    tube['type'] = 'stream'
    tube['service'] = 'echo'
    tube['id'] = str(stream_tube_id)
    parameters = tube.addElement((None, 'parameters'))
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 's'
    parameter['type'] = 'str'
    parameter.addContent('hello')
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 'ay'
    parameter['type'] = 'bytes'
    parameter.addContent('aGVsbG8=')
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 'u'
    parameter['type'] = 'uint'
    parameter.addContent('123')
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 'i'
    parameter['type'] = 'int'
    parameter.addContent('-123')

    stream.send(presence)

    # text channel
    event, new_event = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'))

    assert event.args[1] == cs.CHANNEL_TYPE_TEXT, event.args

    channels = new_event.args[0]
    assert len(channels) == 1
    path, props = channels[0]
    assert props[cs.CHANNEL_TYPE] == cs.CHANNEL_TYPE_TEXT

    # tubes channel is automatically created
    event, new_event = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'))

    assert event.args[1] == cs.CHANNEL_TYPE_TUBES, event.args
    assert event.args[2] == cs.HT_ROOM
    assert event.args[3] == room_handle

    tubes_chan = bus.get_object(conn.bus_name, event.args[0])
    tubes_iface = dbus.Interface(tubes_chan, event.args[1])

    channel_props = tubes_chan.GetAll(cs.CHANNEL, dbus_interface=cs.PROPERTIES_IFACE)
    assert channel_props['TargetID'] == 'chat@conf.localhost', channel_props
    assert channel_props['Requested'] == False
    assert channel_props['InitiatorID'] == ''
    assert channel_props['InitiatorHandle'] == 0

    channels = new_event.args[0]
    assert len(channels) == 1
    path, props = channels[0]
    assert props[cs.CHANNEL_TYPE] == cs.CHANNEL_TYPE_TUBES

    tubes_self_handle = tubes_chan.GetSelfHandle(dbus_interface=cs.CHANNEL_IFACE_GROUP)

    q.expect('dbus-signal', signal='NewTube',
        args=[stream_tube_id, bob_handle, 1, 'echo', sample_parameters, 0])

    expected_tube = (stream_tube_id, bob_handle, cs.TUBE_TYPE_STREAM, 'echo',
        sample_parameters, cs.TUBE_STATE_LOCAL_PENDING)
    tubes = tubes_iface.ListTubes(byte_arrays=True)
    assert tubes == [(
        stream_tube_id,
        bob_handle,
        1,      # Stream
        'echo',
        sample_parameters,
        cs.TUBE_CHANNEL_STATE_LOCAL_PENDING
        )]

    assert len(tubes) == 1, unwrap(tubes)
    t.check_tube_in_tubes(expected_tube, tubes)

    # tube channel is also announced (new API)
    new_event = q.expect('dbus-signal', signal='NewChannels')

    channels = new_event.args[0]
    assert len(channels) == 1
    path, props = channels[0]
    assert props[cs.CHANNEL_TYPE] == cs.CHANNEL_TYPE_STREAM_TUBE
    assert props[cs.INITIATOR_HANDLE] == bob_handle
    assert props[cs.INITIATOR_ID] == 'chat@conf.localhost/bob'
    assert props[cs.INTERFACES] == [cs.CHANNEL_IFACE_GROUP, cs.CHANNEL_IFACE_TUBE]
    assert props[cs.REQUESTED] == False
    assert props[cs.TARGET_HANDLE] == room_handle
    assert props[cs.TARGET_ID] == 'chat@conf.localhost'
    assert props[cs.STREAM_TUBE_SERVICE] == 'echo'
    assert props[cs.TUBE_PARAMETERS] == {'s': 'hello', 'ay': 'hello', 'u': 123, 'i': -123}

    tube_chan = bus.get_object(conn.bus_name, path)
    tube_props = tube_chan.GetAll(cs.CHANNEL_IFACE_TUBE, dbus_interface=cs.PROPERTIES_IFACE,
        byte_arrays=True)
    assert tube_props['Parameters'] == sample_parameters
    assert tube_props['State'] == cs.TUBE_CHANNEL_STATE_LOCAL_PENDING

    # Accept the tube
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id, 0, 0, '',
            byte_arrays=True)

    accept_return_event, _ = q.expect_many(
        EventPattern('dbus-return', method='AcceptStreamTube'),
        EventPattern('dbus-signal', signal='TubeStateChanged',
            args=[stream_tube_id, 2]))

    unix_socket_adr = accept_return_event.value[0]

    factory = EventProtocolClientFactory(q)
    reactor.connectUNIX(unix_socket_adr, factory)

    event = q.expect('socket-connected')
    protocol = event.protocol
    protocol.sendData("hello initiator")

    # expect SI request
    event = q.expect('stream-iq', to='chat@conf.localhost/bob', query_ns=ns.SI,
        query_name='si')

    bytestream, profile = create_from_si_offer(stream, q, bytestream_cls, event.stanza,
            'chat@conf.localhost/test')

    assert profile == ns.TUBES

    muc_stream_node = xpath.queryForNodes('/iq/si/muc-stream[@xmlns="%s"]' %
        ns.TUBES, event.stanza)[0]
    assert muc_stream_node is not None
    assert muc_stream_node['tube'] == str(stream_tube_id)

    # set the real jid of the target as 'to' because the XMPP server changes
    # it when delivering the IQ
    result, si = bytestream.create_si_reply(event.stanza, 'test@localhost/Resource')
    si.addElement((ns.TUBES, 'tube'))
    stream.send(result)

    bytestream.wait_bytestream_open()

    binary = bytestream.get_data()
    assert binary == 'hello initiator'

    # reply on the socket
    bytestream.send_data('hi joiner!')

    q.expect('socket-data', protocol=protocol, data="hi joiner!")

    # OK, we're done
    conn.Disconnect()

    q.expect_many(
        EventPattern('dbus-signal', signal='TubeClosed', args=[stream_tube_id]),
        EventPattern('dbus-signal', signal='StatusChanged', args=[2, 1]))

if __name__ == '__main__':
    t.exec_tube_test(test)