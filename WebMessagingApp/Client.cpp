#include "Client.h"
#include <iostream>

using namespace std;

Client::Client(string nameToSet)
{
	cout << "Client " << nameToSet << " entered." << endl;
	setName(nameToSet);
	isOnline = true; // Default to online status when a client is created
}

string Client::getName() const
{
	return name;
}

void Client::setName(string nameToSet) 
{
	name = nameToSet;
}
