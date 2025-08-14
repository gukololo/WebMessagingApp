#include "Message.h"
#include <iostream>

using namespace std;

Message::Message(string msgToSet, string senderNameToSet, string destinationNameToSet)
{
	setMessage(msgToSet);
	setSenderName(senderNameToSet);
	setDestinationName(destinationNameToSet);
}

void Message::setSenderName(string senderNameToSet)
{
	senderName = senderNameToSet;
}

void Message::setDestinationName(string destinationNameToSet)
{
	destinationName = destinationNameToSet;
}

void Message::setMessage(string msgToSet)
{
	message = msgToSet;
}

string Message::getSenderName() const
{
	return senderName;
}

string Message::getDestinationName() const
{
	return destinationName;
}

string Message::getMessage() const
{
	return message;
}

