#ifndef CLIENT_H
#define CLIENT_H

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

using namespace std;
class Client{

public:

	Client(string nameToSet);
	string getName()const;
	void setName(string nameToSet);
	bool getIsOnline() const { return isOnline; }
	void setIsOnline(bool onlineStatus) { isOnline = onlineStatus; }
	vector<string> getDestinationNames() const { return destinationNames; }
	
	// Adds a destination name to the list if it doesn't already exist
	void addDestinationName(const string& destinationName) {
		if (find(destinationNames.begin(), destinationNames.end(), destinationName) == destinationNames.end()) {
			destinationNames.push_back(destinationName);
		}
	}

	// Removes a destination name from the list if it exists
	void removeDestinationName(const string& destinationName) {
		auto it = find(destinationNames.begin(), destinationNames.end(), destinationName);
		if (it != destinationNames.end()) {
			destinationNames.erase(it);
		}
	}

private:
	string name;
	bool isOnline;
	vector<string> destinationNames;
};

#endif 
