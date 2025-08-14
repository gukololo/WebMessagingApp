#ifndef CLIENT_H
#define CLIENT_H

#include <iostream>
#include <string>

using namespace std;
class Client{

public:
	Client(string nameToSet);
	string getName()const;
	void setName(string nameToSet);
private:
	string name;
};

#endif 
