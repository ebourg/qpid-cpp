/*
 *
 * Copyright (c) 2006 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include "Connection.h"
#include "Channel.h"
#include "ConnectorImpl.h"
#include "Message.h"
#include "QpidError.h"
#include <iostream>

using namespace qpid::client;
using namespace qpid::framing;
using namespace qpid::io;
using namespace qpid::concurrent;

u_int16_t Connection::channelIdCounter;

Connection::Connection(bool debug, u_int32_t _max_frame_size) : max_frame_size(_max_frame_size), closed(true){
    connector = new ConnectorImpl(debug, _max_frame_size);
}

Connection::~Connection(){
    delete connector;
}

void Connection::open(const std::string& _host, int _port, const std::string& uid, const std::string& pwd, const std::string& virtualhost){
    host = _host;
    port = _port;
    connector->setInputHandler(this);
    connector->setTimeoutHandler(this);
    connector->setShutdownHandler(this);
    out = connector->getOutputHandler();
    connector->connect(host, port);
    
    ProtocolInitiation* header = new ProtocolInitiation(8, 0);
    responses.expect();
    connector->init(header);
    responses.receive(connection_start);

    FieldTable props;
    string mechanism("PLAIN");
    string response = ((char)0) + uid + ((char)0) + pwd;
    string locale("en_US");
    responses.expect();
    out->send(new AMQFrame(0, new ConnectionStartOkBody(props, mechanism, response, locale)));

    /**
     * Assume for now that further challenges will not be required
    //receive connection.secure
    responses.receive(connection_secure));
    //send connection.secure-ok
    out->send(new AMQFrame(0, new ConnectionSecureOkBody(response)));
    **/

    responses.receive(connection_tune);

    ConnectionTuneBody::shared_ptr proposal = std::tr1::dynamic_pointer_cast<ConnectionTuneBody, AMQMethodBody>(responses.getResponse());
    out->send(new AMQFrame(0, new ConnectionTuneOkBody(proposal->getChannelMax(), max_frame_size, proposal->getHeartbeat())));

    u_int16_t heartbeat = proposal->getHeartbeat();
    connector->setReadTimeout(heartbeat * 2);
    connector->setWriteTimeout(heartbeat);

    //send connection.open
    string capabilities;
    string vhost = virtualhost;
    responses.expect();
    out->send(new AMQFrame(0, new ConnectionOpenBody(vhost, capabilities, true)));
    //receive connection.open-ok (or redirect, but ignore that for now esp. as using force=true).
    responses.waitForResponse();
    if(responses.validate(connection_open_ok)){
        //ok
    }else if(responses.validate(connection_redirect)){
        //ignore for now
        ConnectionRedirectBody::shared_ptr redirect(std::tr1::dynamic_pointer_cast<ConnectionRedirectBody, AMQMethodBody>(responses.getResponse()));
        std::cout << "Received redirection to " << redirect->getHost() << std::endl;
    }else{
        THROW_QPID_ERROR(PROTOCOL_ERROR, "Bad response");
    }
    
}

void Connection::close(){
    if(!closed){
        u_int16_t code(200);
        string text("Ok");
        u_int16_t classId(0);
        u_int16_t methodId(0);
        
        sendAndReceive(new AMQFrame(0, new ConnectionCloseBody(code, text, classId, methodId)), connection_close_ok);
        connector->close();
    }
}

void Connection::openChannel(Channel* channel){
    channel->con = this;
    channel->id = ++channelIdCounter;
    channel->out = out;
    channels[channel->id] = channel;
    //now send frame to open channel and wait for response
    string oob;
    channel->sendAndReceive(new AMQFrame(channel->id, new ChannelOpenBody(oob)), channel_open_ok);
    channel->setQos();
    channel->closed = false;
}

void Connection::closeChannel(Channel* channel){
    //send frame to close channel
    u_int16_t code(200);
    string text("Ok");
    u_int16_t classId(0);
    u_int16_t methodId(0);
    closeChannel(channel, code, text, classId, methodId);
}

void Connection::closeChannel(Channel* channel, u_int16_t code, string& text, u_int16_t classId, u_int16_t methodId){
    //send frame to close channel
    channel->cancelAll();
    channel->closed = true;
    channel->sendAndReceive(new AMQFrame(channel->id, new ChannelCloseBody(code, text, classId, methodId)), channel_close_ok);
    channel->con = 0;
    channel->out = 0;
    removeChannel(channel);
}

void Connection::removeChannel(Channel* channel){
    //send frame to close channel

    channels.erase(channel->id);
    channel->out = 0;    
    channel->id = 0;
    channel->con = 0;
}

void Connection::received(AMQFrame* frame){
    u_int16_t channelId = frame->getChannel();

    if(channelId == 0){
        this->handleBody(frame->getBody());
    }else{
        Channel* channel = channels[channelId];
        if(channel == 0){
            error(504, "Unknown channel");
        }else{
            try{
                channel->handleBody(frame->getBody());
            }catch(qpid::QpidError e){
                channelException(channel, dynamic_cast<AMQMethodBody*>(frame->getBody().get()), e);
            }
        }
    }
}

void Connection::handleMethod(AMQMethodBody::shared_ptr body){
    //connection.close, basic.deliver, basic.return or a response to a synchronous request
    if(responses.isWaiting()){
        responses.signalResponse(body);
    }else if(connection_close.match(body.get())){
        //send back close ok
        //close socket
        ConnectionCloseBody* request = dynamic_cast<ConnectionCloseBody*>(body.get());
        std::cout << "Connection closed by server: " << request->getReplyCode() << ":" << request->getReplyText() << std::endl;
        connector->close();
    }else{
        std::cout << "Unhandled method for connection: " << *body << std::endl;
        error(504, "Unrecognised method", body->amqpClassId(), body->amqpMethodId());
    }
}
    
void Connection::handleHeader(AMQHeaderBody::shared_ptr /*body*/){
    error(504, "Channel error: received header body with channel 0.");
}
    
void Connection::handleContent(AMQContentBody::shared_ptr /*body*/){
    error(504, "Channel error: received content body with channel 0.");
}
    
void Connection::handleHeartbeat(AMQHeartbeatBody::shared_ptr /*body*/){
}

void Connection::sendAndReceive(AMQFrame* frame, const AMQMethodBody& body){
    responses.expect();
    out->send(frame);
    responses.receive(body);
}

void Connection::error(int code, const string& msg, int classid, int methodid){
    std::cout << "Connection exception generated: " << code << msg;
    if(classid || methodid){
        std::cout << " [" << methodid << ":" << classid << "]";
    }
    std::cout << std::endl;
    sendAndReceive(new AMQFrame(0, new ConnectionCloseBody(code, msg, classid, methodid)), connection_close_ok);
    connector->close();
}

void Connection::channelException(Channel* channel, AMQMethodBody* method, QpidError& e){
    std::cout << "Caught error from channel [" << e.code << "] " << e.msg << " (" << e.file << ":" << e.line << ")" << std::endl;
    int code = e.code == PROTOCOL_ERROR ? e.code - PROTOCOL_ERROR : 500;
    string msg = e.msg;
    if(method == 0){
        closeChannel(channel, code, msg);
    }else{
        closeChannel(channel, code, msg, method->amqpClassId(), method->amqpMethodId());
    }
}

void Connection::idleIn(){
    std::cout << "Connection timed out due to abscence of heartbeat." << std::endl;
    connector->close();
}

void Connection::idleOut(){
    out->send(new AMQFrame(0, new AMQHeartbeatBody()));
}

void Connection::shutdown(){
    closed = true;
    //close all channels
    for(iterator i = channels.begin(); i != channels.end(); i++){
        i->second->stop();
    }
}
