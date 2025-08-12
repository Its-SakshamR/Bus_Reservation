#include <iostream>
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>
using namespace std;
using namespace pqxx;

string sha256(const string& str) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)str.c_str(), str.size(), hash);

    stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        ss << hex << setw(2) << setfill('0') << (int)hash[i];
    return ss.str();
}

class BusReservationSystem {
private:
    connection conn;

public:
    BusReservationSystem(const string& connStr)
        : conn(connStr)
    {
        if (!conn.is_open()) {
            throw runtime_error("Cannot connect to DB.");
        }
    }

    void registerUser(const string& username, const string& password)
    {
        work txn(conn);
        string hashed = sha256(password);
        txn.exec_params("INSERT INTO users(username, password) VALUES($1, $2)", username, hashed);
        txn.commit();
        cout << "\n\t\tUser registered successfully.\n";
    }

    bool authenticateUser(const string& username, const string& password)
    {
        work txn(conn);
        string hashed = sha256(password);
        result r = txn.exec_params(
            "SELECT * FROM users WHERE LOWER(username) = LOWER($1) AND password = $2",
            username, hashed);
        return !r.empty();
    }

    int getUserId(const string& username)
    {
        work txn(conn);
        result r = txn.exec_params("SELECT id FROM users WHERE username = $1", username);
        if (r.empty())
            throw runtime_error("User not found");
        return r[0][0].as<int>();
    }

    void displayRoutes()
    {
        work txn(conn);
        result r = txn.exec("SELECT * FROM routes");
        cout << "\n\t\t------ Available Routes ------\n";
        for (auto row : r) {
            cout << "\n\t\tRoute Name: " << row["name"].c_str()
                 << " (" << row["source"].c_str()
                 << " to " << row["destination"].c_str() << ")\n";
            cout << "\t\t\tDistance: " << row["distance"].as<int>() << " km\n";
        }
    }

    void displayBuses()
    {
        string routeName;
        cout << "\n\n\t\tEnter route name (e.g., R001): ";
        cin >> routeName;

        work txn(conn);

        result routeResult = txn.exec_params(
            "SELECT id, source, destination FROM routes WHERE name = $1",
            routeName
        );

        if (routeResult.empty()) {
            cout << "Invalid route name. Returning to previous menu." << endl;
            return;
        }

        int routeId = routeResult[0]["id"].as<int>();

        // Fetch buses for this route
        result buses = txn.exec_params(
            "SELECT buses.bus_number, routes.name, routes.source, routes.destination, routes.distance, buses.total_seats "
            "FROM buses JOIN routes ON buses.route_id = routes.id WHERE buses.route_id = $1",
            routeId
        );

        if (buses.empty()) {
            cout << "No buses available for route " << routeName << " (" << routeResult[0]["source"].as<string>() << " → " << routeResult[0]["destination"].as<string>() << ")." << endl;
            return;
        }

        cout << "\n\t\t------ Available Buses ------\n";
        for (auto row : buses) {
            cout << "\n\t\tBus Number: " << row["bus_number"].c_str() << "\n";
            cout << "\t\tRoute: " << row["name"].c_str()
                 << " (" << row["source"].c_str() << " to " << row["destination"].c_str() << ")\n";
            cout << "\t\tDistance: " << row["distance"].as<int>() << " km\n";
            cout << "\t\tTotal Seats: " << row["total_seats"].as<int>() << "\n";

            // Show seat availability
            string busNumber = row["bus_number"].c_str();
            result seatResult = txn.exec_params(
                "SELECT seat_number, is_reserved FROM seats WHERE bus_id = "
                "(SELECT id FROM buses WHERE bus_number = $1) ORDER BY seat_number",
                busNumber);
            cout << "\t\t----- Seat Status -----\n\t\t";
            int count = 0;
            for (auto seat : seatResult) {
                bool reserved = seat["is_reserved"].as<bool>();
                cout << (reserved ? "NA" : to_string(seat["seat_number"].as<int>())) << "\t";
                if (++count % 4 == 0) cout << "\n\t\t";
            }
            cout << "\n";
        }
    }

    void bookTicket(const string& username)
    {
        string routeName, busNumber;
        int seatNumber;

        displayRoutes();
        cout << "\n\t\tEnter Route Name: ";
        cin >> routeName;

        work txn(conn);
        result routeRes = txn.exec_params("SELECT id FROM routes WHERE name = $1", routeName);
        if (routeRes.empty()) {
            cout << "\n\tInvalid route name.\n";
            return;
        }
        int routeId = routeRes[0][0].as<int>();

        result busRes = txn.exec_params("SELECT bus_number FROM buses WHERE route_id = $1", routeId);
        if (busRes.empty()) {
            cout << "\n\tNo buses available for this route.\n";
            return;
        }

        cout << "\n\t\tAvailable Buses for " << routeName << ":\n";
        for (auto row : busRes) {
            cout << "\t\tBus Number: " << row["bus_number"].c_str() << "\n";
        }

        cout << "\n\t\tEnter Bus Number: ";
        cin >> busNumber;

        result busIdRes = txn.exec_params("SELECT id FROM buses WHERE bus_number = $1 AND route_id = $2", busNumber, routeId);
        if (busIdRes.empty()) {
            cout << "\n\tInvalid Bus Number.\n";
            return;
        }
        int busId = busIdRes[0][0].as<int>();

        result seatRes = txn.exec_params("SELECT seat_number FROM seats WHERE bus_id = $1 AND is_reserved = false", busId);
        if (seatRes.empty()) {
            cout << "\n\tNo seats available.\n";
            return;
        }

        cout << "\n\t\tAvailable Seats:\n\t\t";
        for (auto row : seatRes) {
            cout << row["seat_number"].as<int>() << " ";
        }
        cout << "\n\t\tEnter Seat Number to Book: ";
        cin >> seatNumber;
        txn.commit();

        // Reserve seat and insert ticket
        try {
            int userId = getUserId(username);
            work txn2(conn);

            // Find bus ID from bus_number
            result busRes = txn2.exec_params(
                "SELECT id FROM buses WHERE bus_number = $1",
                busNumber
            );

            if (busRes.empty()) {
                cout << "Invalid bus number.\n";
                return;
            }

            int busId = busRes[0]["id"].as<int>();

            // Check if seat exists and is available
            result seatRes = txn2.exec_params(
                "SELECT id, is_reserved FROM seats WHERE bus_id = $1 AND seat_number = $2",
                busId, seatNumber
            );

            if (seatRes.empty()) {
                cout << "Invalid seat number for the selected bus.\n";
                return;
            }

            bool isReserved = seatRes[0]["is_reserved"].as<bool>();
            int seatId = seatRes[0]["id"].as<int>();

            if (isReserved) {
                cout << "Seat is already reserved.\n";
                return;
            }

            // Reserve seat and insert ticket
            txn2.exec_params(
                "UPDATE seats SET is_reserved = TRUE WHERE id = $1",
                seatId
            );
            
            txn2.exec_params(
                "INSERT INTO tickets (user_id, bus_id, seat_number) VALUES ($1, $2, $3)",
                userId, busId, seatNumber
            );

            txn2.commit();

            cout << "Ticket booked successfully for seat number " << seatNumber << " on bus " << busNumber << ".\n";

        } catch (const std::exception& e) {
            cerr << "Error booking ticket: " << e.what() << endl;
        }
    }

    void cancelTicket(const string& username)
    {
        try {
            int userId = getUserId(username);
            work txn(conn);
            result tickets = txn.exec_params(
                "SELECT t.id, b.bus_number, t.seat_number FROM tickets t "
                "JOIN buses b ON t.bus_id = b.id WHERE t.user_id = $1",
                userId);

            if (tickets.empty()) {
                cout << "\n\tNo tickets found.\n";
                return;
            }

            cout << "\n\tYour Tickets:\n";
            for (int i = 0; i < tickets.size(); ++i) {
                cout << "\t" << (i + 1) << ". Bus: " << tickets[i]["bus_number"].c_str()
                     << ", Seat: " << tickets[i]["seat_number"].as<int>() << "\n";
            }

            cout << "\n\tEnter ticket number to cancel: ";
            int ticketIndex;
            cin >> ticketIndex;

            if (ticketIndex < 1 || ticketIndex > tickets.size()) {
                cout << "\n\tInvalid selection.\n";
                return;
            }

            int ticketId = tickets[ticketIndex - 1]["id"].as<int>();
            int seatNumber = tickets[ticketIndex - 1]["seat_number"].as<int>();
            string busNumber = tickets[ticketIndex - 1]["bus_number"].c_str();

            txn.commit();

            work txn2(conn);
            txn2.exec_params(
                "UPDATE seats SET is_reserved = false "
                "WHERE seat_number = $1 AND bus_id = "
                "(SELECT id FROM buses WHERE bus_number = $2)",
                seatNumber, busNumber);
            txn2.exec_params("DELETE FROM tickets WHERE id = $1", ticketId);
            txn2.commit();
            cout << "\n\tTicket cancelled successfully.\n";
        } catch (const exception& e) {
            cerr << "\n\tError: " << e.what() << "\n";
        }
    }

    void run()
    {
        while (true) {
            cout << "\n\t------ Welcome to the Bus Reservation System ------\n\n";
            cout << "\t\t1. Register\n\t\t2. Log In\n\t\t3. Exit\n";
            cout << "\n\t\tEnter your choice: ";
            int choice;
            cin >> choice;

            if (choice == 1) {
                string username, password;
                cout << "\n\tEnter username: ";
                cin >> username;
                cout << "\tEnter password: ";
                cin >> password;
                registerUser(username, password);
            }
            else if (choice == 2) {
                string username, password;
                cout << "\n\tEnter username: ";
                cin >> username;
                cout << "\tEnter password: ";
                cin >> password;
                if (authenticateUser(username, password)) {
                    cout << "\n\tLogin successful!\n";
                    userMenu(username);
                }
                else {
                    cout << "\n\tInvalid username or password.\n";
                }
            }
            else if (choice == 3) {
                cout << "\n\tExiting... Goodbye!\n";
                break;
            }
        }
    }

    void userMenu(const string& username)
    {
        while (true) {
            cout << "\n\t------ User Menu ------\n";
            cout << "\t1. Display Routes\n\t2. Display Buses\n\t3. Book Ticket\n\t4. Cancel Ticket\n\t5. Logout\n";
            cout << "\n\tEnter your choice: ";
            int choice;
            cin >> choice;

            switch (choice) {
                case 1:
                    displayRoutes();
                    break;
                case 2:
                    displayBuses();
                    break;
                case 3:
                    bookTicket(username);
                    break;
                case 4:
                    cancelTicket(username);
                    break;
                case 5:
                    cout << "\n\tLogged out.\n";
                    return;
                default:
                    cout << "\n\tInvalid choice.\n";
            }
        }
    }
};


int main()
{
    try {
        string connStr = "dbname=bus_reservation_db user=saksham-raj password=saksham";
        BusReservationSystem system(connStr);
        system.run();
    }
    catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
    }
    return 0;
}
