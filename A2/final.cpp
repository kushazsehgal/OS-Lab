
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <termios.h>
#include <sys/inotify.h>
#include <glob.h>
#include <map>
using namespace std;

#define GREEN "\033[1;32m"
#define BLUE "\033[1;34m"
#define RESET "\033[0m"

bool ongoing = false;
int run_in_background = 0;
int inotify_fd;

vector <string> history;
int ind = 0;
bool up = false;
vector<string> temp_files;
string command_executing = "";
map<pid_t, int> pid_to_index;

void print_prompt(){
    char hostname[1024];
    gethostname(hostname, 1024);
    char cwd[1024];
    getcwd(cwd, 1024);
    cout << GREEN << hostname << RESET << ":" << BLUE << cwd << RESET << "$ ";
}

struct termios old_tio;

void setup_terminal(){
    struct termios new_tio;
    tcgetattr(0, &old_tio);     /* grab old terminal i/o settings */
    tcgetattr(0, &new_tio);     /* grab old terminal i/o settings */
    new_tio.c_lflag &= ~(ICANON|ECHO);  /* disable buffered i/o */
    new_tio.c_cc[VMIN] = 1;             /* minimum # of characters to read */
    new_tio.c_cc[VTIME] = 0;            
    tcsetattr(0, TCSANOW, &new_tio);    /* use these new terminal i/o settings now */
}

void reset_terminal(){
    old_tio.c_lflag |= (ICANON|ECHO);  /* disable buffered i/o */
    old_tio.c_cc[VMIN] = 0;             /* minimum # of characters to read */
    old_tio.c_cc[VTIME] = 1;
    tcsetattr(0, TCSANOW, &old_tio);    /* use these new terminal i/o settings now */
}
string trim(string s){
    int i = 0;
    while(s[i] == ' '){
        i++;
    }
    if(i == s.length()){
        return "";
    }
    s = s.substr(i);
    while(s.back() == ' '){
        s.pop_back();
    }
    return s;
}

vector<string> split(string s, char delim){
    vector<string> v;
    int last = 0;
    for(int i=0; i<s.length(); i++){
        if(s[i] == delim){
            string temp = trim(s.substr(last, i-last));
            if(!temp.empty()){
                v.push_back(temp);
            }
            last = i+1;
        }
    }
    string temp = trim(s.substr(last));
    if(!temp.empty()){
        v.push_back(temp);
    }
    return v;
}

vector<string> handle_input_output_redirection(string cmd){
    bool inp_redir = (cmd.find('<') != string::npos);
    bool out_redir = (cmd.find('>') != string::npos);
    if(!inp_redir && !out_redir){
        return {cmd, "", ""};
    }else if(!inp_redir){
        vector<string> v = split(cmd, '>');
        return {v[0], "", v[1]};
    }else if(!out_redir){
        vector<string> v = split(cmd, '<');
        return {v[0], v[1], ""};
    }else{
        vector<string> v = split(cmd, '<');
        if(v[0].find('>') != string::npos){
            vector<string> v2 = split(v[0], '>');
            return {v2[0], v[1], v2[1]};
        }else{
            vector<string> v2 = split(v[1], '>');
            return {v[0], v2[0], v2[1]};
        }
    }
}

void redirect_in_out(string inp, string out){
    if(!inp.empty()){
        int fd = open(inp.c_str(), O_RDONLY);
        if(fd == -1){
            cout<<"Error: No such file or directory"<<endl;
            exit(1);
        }
        if(dup2(fd, 0) == -1){
            cout<<"Error: Redirection error"<<endl;
            exit(1);
        }
    }
    if(!out.empty()){
        int fd = open(out.c_str(), O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
        if(fd == -1){
            cout<<"Error: No such file or directory"<<endl;
            exit(1);
        }
        if(dup2(fd, 1) == -1){
            cout<<"Error: Redirection error"<<endl;
            exit(1);
        }
    }
}

void execute_command(vector<string> args){
    if(args.empty()){
        return;
    }
    // expand wildcards
    for(int i=0; i<args.size(); i++){
        if(args[i].find('*') != string::npos || args[i].find('?') != string::npos){ // if globbing is required
            glob_t glob_result; // glob_result is a structure that contains the list of files that match the pattern
            glob(args[i].c_str(), GLOB_TILDE, NULL, &glob_result); // GLOB_TILDE is used to expand ~ to home directory
            for(int j=0; j<glob_result.gl_pathc; j++){ // gl_pathc is the number of files that match the pattern
                args.push_back(glob_result.gl_pathv[j]); // gl_pathv is the list of files that match the pattern
            } 
            args.erase(args.begin()+i); // remove the pattern from the arguments
        }
    }
    char* argv[args.size()+1];
    for(int i=0; i<args.size(); i++){
        argv[i] = (char*)args[i].c_str();
    }
    argv[args.size()] = NULL;
    char* const* argv_dup = (char* const*)argv;
    command_executing= argv[0];
    execvp(argv[0], argv_dup);
    cout<<"Error: Command not found"<<endl;
    exit(1);
}

void delep(const char* filepath){
    // spwan a child process to list all processes using the file
    int fd[2];
    pipe(fd);
    pid_t pid = fork();
    if(pid == 0){
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execlp("lsof", "lsof", filepath, NULL);
        exit(1);
    }
    close(fd[1]);
    string output;
    char c;
    while(read(fd[0], &c, 1) > 0){
        output += c;
    }
    close(fd[0]);
    int status;
    waitpid(pid, &status, 0);
    vector<string> lines = split(output, '\n');
    vector<int> pids;
    for(int i=1; i<lines.size(); i++){
        vector<string> words = split(lines[i], ' ');
        pids.push_back(stoi(words[1]));
    }
    cout<<"Processes using the file: ";
    for(auto pid: pids){
        cout<<pid<<" ";
    }cout<<endl;
    char user_input;
    cout<<"Kill Processes using the file? (y/n): ";
    cin>>user_input;
    if(user_input == 'y'){
        for(int i=0; i<pids.size(); i++){
            kill(pids[i], SIGKILL);
        }
    }else if(user_input == 'n'){
        return;
    }else{
        cout<<"Invalid input"<<endl;
        return;
    }
}   
void process_command(string cmd){
    run_in_background = 0;
    int status = 0;
    if(cmd.back() == '&'){
        run_in_background = 1;
        cmd.back() = ' ';
    }
    bool is_pipe = (cmd.find('|') != string::npos);
    if(is_pipe){
        vector<string> commands = split(cmd, '|');
        int pp[2], pp_old[2];
        for(int i=0; i<commands.size(); i++){
            vector<string> redirections = handle_input_output_redirection(commands[i]);
            if(i!=commands.size()-1){
                pipe(pp);
            }
            pid_t pid = fork();
            if(pid == 0){
                if(i == 0 || i == commands.size()-1){
                    redirect_in_out(redirections[1], redirections[2]);
                }
                if(i != 0){
                    dup2(pp_old[0], 0);
                    close(pp_old[0]);
                    close(pp_old[1]);
                }
                if(i != commands.size()-1){
                    close(pp[0]);
                    dup2(pp[1], 1);
                    close(pp[1]);
                }
                vector<string> args = split(redirections[0], ' ');
                execute_command(args);
            }
            if(i != 0){
                close(pp_old[0]);
                close(pp_old[1]);
            }
            if(i != commands.size()-1){
                pp_old[0] = pp[0];
                pp_old[1] = pp[1];
            }
            if(!run_in_background){
                waitpid(pid, &status, WUNTRACED);
                if(WIFSTOPPED(status)){
                    kill(pid, SIGCONT);
                }
            }
        }
    }else{
        if(cmd.find("cd") != string::npos){
            vector<string> args = split(cmd, ' ');
            if(args.size() == 1){
                chdir(getenv("HOME"));
            }else{
                for(int i=2; i<args.size(); i++){
                    args[1] += " " + args[i];
                }
                chdir(args[1].c_str());
            }
            return;
        }
        else if(cmd.find("delep") != string::npos){
            vector<string> args = split(cmd, ' ');
            if(args.size() == 1){
                cout<<"Usage: delep <filepath>"<<endl;
            }else{
                for(int i=2; i<args.size(); i++){
                    args[1] += " " + args[i];
                }
                const char* file_path = args[1].c_str();
                delep(file_path);
            }
            return;
        }
        vector<string> redirections = handle_input_output_redirection(cmd);
        vector<string> args = split(redirections[0], ' ');
        pid_t pid = fork();
        if(pid == 0){
            redirect_in_out(redirections[1], redirections[2]);
            execute_command(args);
        }
        if(!run_in_background){
            pid_to_index[pid] = history.size();
            ongoing = true;
            waitpid(pid, &status, WUNTRACED);
            pid_to_index.erase(pid);
            if(WIFSTOPPED(status)){
                kill(pid, SIGCONT);
            }
        }   
    }

}
void signal_handler_CtrlC(int signum){
    cout << endl;
    if(!ongoing)
        print_prompt();
    // halt the current process
    fflush(stdout);
}

void signal_handler_CtrlZ(int signum){
    cout<<endl;
    // cout<<"[1]+ Stopped "<<command_executing<<endl;
    print_prompt();
    fflush(stdout);
    vector<int> killed;
    for(auto it: pid_to_index){
        cout<<"["<<it.second+1<<"]+ Stopped "<<endl;
        kill(it.first, SIGTSTP);
        killed.push_back(it.first);
    }
    
    for(auto pid: killed){
        pid_to_index.erase(pid);
    }

    // if(!run_in_background){
    //     run_in_background = 1;
    // }
}

int main(){
    signal(SIGINT, signal_handler_CtrlC);
    signal(SIGTSTP, signal_handler_CtrlZ);
    // check if the history file exists
    ifstream command_history;
    command_history.open("history.txt");
    if(command_history.is_open()){
        string line;
        while(getline(command_history, line)){
            history.push_back(line);
        }
        command_history.close();
    }
    ind = history.size();

    while(1){
        print_prompt();
        string cmd;
        // getline(cin, cmd);
        while(1){
            setup_terminal();
            char c = getchar();
            if(c == 10) break;//10 --> ascii for newline
            else if(c == 127){//127 --> ascii for backspace
                ind = history.size();
                if(cmd.size() > 0){
                    cmd.pop_back();
                    cout<<"\b \b";
             }  
            }
            else if(c == 27){//27 --> ascii for escape
                c = getchar();
                c = getchar();
                if(history.empty()) continue;
                if(c=='A'){
                    if(ind==-1){
                        ind = history.size();
                    }
                    up = true;
                    //up arrow
                    while(cmd.size() > 0){
                        cmd.pop_back();
                        cout<<"\b \b";
                    }
                    if(ind >= 0){
                        cout<<history[ind-1];
                        cmd = history[ind-1];
                        ind--;
                    }
                    
                }
                else if(c=='B'){
                    //down arrow
                    if(ind < history.size()-1){
                        while(cmd.size() > 0){
                        cmd.pop_back();
                        cout<<"\b \b";
                    }
                        ind++;
                        cout<<history[ind];
                        cmd = history[ind];
                    
                    }
                    
                }
                else ind = history.size();
            }
            else if(c == 4){
                cout<<"exit"<<endl;
                reset_terminal();
                ofstream save_history;
                save_history.open("history.txt", ios::out | ios::trunc);
                for(int i=0; i<history.size(); i++){
                    save_history << history[i] << endl;
                }
                save_history.close();
                return 0;
            }
            else{
                ind = history.size();
                if(c!=3 && c!=26)
                    cmd += c;
                cout<<c;
            }
        }
        cout<<endl;
        if(cmd == "exit"){
            break;
        }
        history.push_back(cmd);
        ind++;
        cmd = trim(cmd);
        reset_terminal();
        if(cmd == "exit"){
            reset_terminal();
            return 0;
        }
        process_command(cmd);
        ongoing = false;
    }
    reset_terminal();
    ofstream save_history;
    save_history.open("history.txt", ios::out | ios::trunc);
    for(int i=0; i<history.size(); i++){
        save_history << history[i] << endl;
    }
    save_history.close();
    return 0;
}