#ifndef MESSAGE_H
#define MESSAGE_H
#include <iostream>
#include <string>

using namespace std;

class Message {

private:
	string senderName;
	string destinationName;
	string message;
public:
	Message(string msgToSet, string senderNameToSet, string destinationNameToSet);
	void setSenderName(string senderNameToSet);
	void setDestinationName(string destinationNameToSet);
	void setMessage(string msgToSet);
	string getSenderName()const;
	string getDestinationName()const;
	string getMessage()const;
};
#endif