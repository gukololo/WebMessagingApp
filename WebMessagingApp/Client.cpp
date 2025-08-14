#include "Client.h"
using namespace std;

Client::Client(string nameToSet)
{
	cout << "Client " << nameToSet << " entered." << endl;
	setName(nameToSet);
}

string Client::getName() const
{
	return name;
}

void Client::setName(string nameToSet) 
{
	name = nameToSet;
}
